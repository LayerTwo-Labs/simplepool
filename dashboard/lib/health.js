/* Hard-failure health checks.
 *
 * Every problem this pool has hit was found by someone running a query, never
 * by being told. These are the conditions that mean money or data is already
 * wrong, evaluated on every page load and surfaced as a banner.
 *
 * Deliberately NOT here: degradation signals (BMM capture, settlement
 * latency, backlog size, reserve headroom). They are worth watching but they
 * fluctuate, and a banner that is sometimes red for a slow afternoon is a
 * banner nobody reads. Everything below is binary and actionable.
 *
 * Each check is independent and self-contained: a check that throws because
 * its table predates the current schema reports `unavailable` rather than
 * taking the page down with it.
 *
 * Evaluated on a timer, not per request. better-sqlite3 is synchronous, and
 * the duplicate-hash check measured 2.0s against 337k shares — running that on
 * the request path with a 15s auto-refresh would stall the whole dashboard.
 * The timer keeps full-history coverage (a windowed check would go green on a
 * duplicate from two days ago that nobody noticed) while page loads only read
 * the last snapshot. See startHealthMonitor / currentHealth.
 */

import { rateVerification } from './stats.js';

/* A payout batch legitimately waits for a Thunder block, which waits for a
 * mainchain block carrying its BMM commitment — routinely ~10 minutes on
 * drynet3. An hour means something is genuinely wrong, not slow. */
const PAYOUT_STALL_SEC = 3600;

/* The block subsidy at a height, on the standard schedule: 50 BTC, halved
 * every 210,000 blocks. Chains derived from Bitcoin — mainnet, testnet,
 * signet, and forknets that keep their parent's height — all share it;
 * regtest's 150-block interval is the exception, and a regtest pool will
 * report this check as failing, which is the correct thing for it to say
 * rather than silently accepting any number.
 *
 * Returns 0 past the last era, where there is nothing left to check. */
const HALVING_INTERVAL = 210000;
export function subsidyAt(height) {
    if (!Number.isFinite(height) || height < 0) return 0;
    const era = Math.floor(height / HALVING_INTERVAL);
    if (era >= 64) return 0;
    return Math.floor(50e8 / Math.pow(2, era));
}

function one(d, sql, ...args) {
    return d.prepare(sql).get(...args);
}

/* Run a check, converting a schema mismatch into "unavailable" rather than an
 * exception. A check that cannot run is not a check that passed. */
function guard(id, label, fn) {
    try {
        const r = fn();
        return r ? { id, label, ...r } : { id, label, ok: true };
    } catch (e) {
        return { id, label, ok: true, unavailable: true, detail: e.message };
    }
}

export function health(handle) {
    const d = !handle ? null
            : (typeof handle.get === 'function' ? handle.get() : handle);
    if (!d) {
        return { ok: false, db_ready: false, checks: [], failing: [],
                 unavailable: [] };
    }

    const checks = [];

    /* Accepted work that never reached the DB. The miner was already told
     * "accepted", so this is an unrecoverable shortfall against them — and no
     * other query can see it, because the rows were never written. */
    checks.push(guard('events_lost', 'Shares accepted but never stored', () => {
        const r = one(d, 'SELECT events_lost AS n FROM pool_meta WHERE id = 1');
        const n = Number(r?.n || 0);
        return { ok: n === 0, value: n,
                 detail: n > 0
                    ? `${n} accepted event(s) lost to failed commits; those shares are uncredited`
                    : null };
    }));

    /* The extranonce1 collision class: two connections rendering identical
     * coinbases and submitting the same hash, credited twice.
     *
     * COUNT(block_hash) not COUNT(*) — the latter counts NULL hashes from
     * legacy rows that COUNT(DISTINCT ...) ignores, which reports every one of
     * them as a duplicate. */
    checks.push(guard('duplicate_shares', 'Duplicate share hashes', () => {
        const r = one(d, `SELECT COUNT(block_hash) - COUNT(DISTINCT block_hash) AS n
                            FROM shares`);
        const n = Number(r?.n || 0);
        return { ok: n === 0, value: n,
                 detail: n > 0
                    ? `${n} share(s) credited more than once — every PPS figure is inflated`
                    : null };
    }));

    /* The three ledger checks from the audit: a credit that does not follow
     * from its own stored rate, a published rate that does not follow from its
     * template, and a credit at a rate never published. */
    checks.push(guard('ledger', 'Ledger arithmetic and rate provenance', () => {
        const v = rateVerification(d);
        if (!v) return { ok: true, unavailable: true, detail: 'DB predates the rate audit' };
        const bad = [];
        if (v.mismatched > 0) bad.push(`${v.mismatched} credit(s) ≠ difficulty × rate_used`);
        if (v.rates_inconsistent > 0) bad.push(`${v.rates_inconsistent} published rate(s) not derivable from their template`);
        if (v.orphaned > 0) bad.push(`${v.orphaned} share(s) credited at an unpublished rate`);
        return { ok: bad.length === 0, value: v.mismatched + v.rates_inconsistent + v.orphaned,
                 detail: bad.length ? bad.join('; ') : null };
    }));

    /* Mined minus owed. Negative means the pool cannot cover its PPS
     * liability out of what it has actually earned.
     *
     * CONFIRMED ONLY. A row in blocks_found is a block candidate: submitblock
     * may have refused it, or the chain may have reorged it out, and either
     * way it pays nothing. Summing every row credited the pool with revenue
     * that never existed and made this check — the one thing standing between
     * an operator and paying out more than was ever mined — permanently and
     * silently green.
     *
     * A young pps-classic pool will read red here: it credits shares before
     * its first confirmed block, so the margin is legitimately negative until
     * one lands. That is the honest number, not a fault in the check. */
    checks.push(guard('margin', 'Pool solvency', () => {
        const r = one(d, `
            SELECT (SELECT COALESCE(SUM(reward_sats),0) + COALESCE(SUM(fee_sats),0)
                      FROM blocks_found WHERE status = 'confirmed')
                 - (SELECT COALESCE(SUM(credited_sats),0) FROM shares) AS margin`);
        const m = Number(r?.margin || 0);
        return { ok: m >= 0, value: m,
                 detail: m < 0
                    ? `owed ${(-m / 1e8).toFixed(4)} BTC more than mined`
                    : null };
    }));

    /* Does the block value the pool is being told make arithmetic sense?
     *
     * A template reports the coinbase value in one field and the per-transaction
     * fees in another, and the proxy reads them independently — so
     * value - fees is an independent estimate of the block subsidy, and the
     * subsidy is not a free parameter. It is 50 BTC halved once per era, and
     * nothing else. If that difference is not a halving value, the pool is
     * being told a block is worth something it is not.
     *
     * This matters far beyond reporting. On the coinbasevalue path the proxy
     * builds the coinbase from this number, so getting it wrong produces a
     * consensus-invalid block the node refuses. On the coinbasetxn path (the
     * CUSF enforcer, where the backend supplies the coinbase) the block stays
     * valid and the error is silent — but the number still sets the PPS rate
     * every share is credited at, so an inflated value overpays every miner by
     * the same factor, and the pool owes real money it never earned.
     *
     * The two directions are both real failure modes of the same parse:
     * value far ABOVE the subsidy means the value field is being over-read,
     * and value at or near ZERO means it is being read as fees-only. */
    checks.push(guard('block_value', 'Block value matches the subsidy', () => {
        const rows = d.prepare(`
            SELECT height, coinbase_value_sats, tx_fees_sats
              FROM templates
             WHERE height > 0
             ORDER BY id DESC LIMIT 50`).all();
        if (rows.length === 0) return { ok: true, value: null, detail: 'no templates yet' };
        let worst = null;
        for (const r of rows) {
            const expected = subsidyAt(Number(r.height));
            if (expected <= 0) continue;   /* past the last halving: nothing to check */
            const implied = Number(r.coinbase_value_sats) - Number(r.tx_fees_sats);
            const ratio = implied / expected;
            /* Fees above the subsidy are possible but rare; 2x is generous.
             * Below the subsidy is not possible at all. */
            if (ratio >= 1 && ratio <= 2) continue;
            if (!worst || Math.abs(Math.log(ratio || 1e-9)) > Math.abs(Math.log(worst.ratio || 1e-9))) {
                worst = { height: Number(r.height), implied, expected, ratio };
            }
        }
        if (!worst) return { ok: true, value: null, detail: null };
        const btc = n => (n / 1e8).toFixed(4);
        return {
            ok: false,
            value: Math.round(worst.ratio * 100) / 100,
            detail: `at height ${worst.height} the template implies a ` +
                    `${btc(worst.implied)} BTC subsidy, but the schedule says ` +
                    `${btc(worst.expected)} BTC — every reward and the PPS rate ` +
                    `are scaled by this`,
        };
    }));

    /* How many of the pool's recent candidates actually made it into the
     * chain. On the alphanet forknet this was effectively 0%, invisible
     * because every candidate was counted as a block — the pool looked like
     * it was winning constantly while earning nothing. A sustained high
     * orphan rate is a real operational signal (a slow or badly-peered node,
     * a template far behind the tip), not chain noise to be swallowed.
     *
     * Only settled candidates count toward the ratio: 'pending' means nothing
     * has been able to verify it yet, which is not evidence either way. */
    checks.push(guard('orphan_rate', 'Blocks reaching the chain', () => {
        const r = one(d, `
            SELECT COUNT(*) FILTER (WHERE status = 'confirmed') AS good,
                   COUNT(*) FILTER (WHERE status IN ('orphaned','rejected')) AS lost
              FROM (SELECT status FROM blocks_found
                     WHERE status <> 'pending'
                     ORDER BY ts DESC LIMIT 100)`);
        const good = Number(r?.good || 0);
        const lost = Number(r?.lost || 0);
        const settled = good + lost;
        /* Nothing settled yet is not a failure — say so rather than
         * reporting a 0% success rate the pool has not earned. */
        if (settled === 0) return { ok: true, value: null, detail: 'no settled candidates yet' };
        const pct = (good / settled) * 100;
        return {
            ok: pct >= 50,
            value: Math.round(pct),
            detail: pct >= 50
                ? null
                : `only ${good} of the last ${settled} candidates reached the chain`,
        };
    }));

    /* In-flight rows with no txid mean the worker died around a broadcast and
     * cannot tell whether it went out. Never auto-resolves — the one state
     * that genuinely needs a human. */
    checks.push(guard('payout_ambiguous', 'Payouts stuck without a txid', () => {
        const r = one(d, `SELECT COUNT(*) AS n FROM payouts_in_flight
                           WHERE txid IS NULL OR txid = ''`);
        const n = Number(r?.n || 0);
        return { ok: n === 0, value: n,
                 detail: n > 0
                    ? `${n} in-flight row(s) with no txid — needs manual reconciliation (payout/README.md)`
                    : null };
    }));

    /* A batch waiting far longer than a Thunder block should take. */
    checks.push(guard('payout_stalled', 'Payout settling', () => {
        const r = one(d, `SELECT COALESCE(MAX(strftime('%s','now') - started_at), 0) AS age,
                                 COUNT(*) AS n
                            FROM payouts_in_flight WHERE txid IS NOT NULL AND txid <> ''`);
        const age = Number(r?.age || 0);
        const n   = Number(r?.n || 0);
        if (n === 0) return { ok: true, value: 0 };
        return { ok: age < PAYOUT_STALL_SEC, value: age,
                 detail: age >= PAYOUT_STALL_SEC
                    ? `a batch has been unconfirmed for ${Math.floor(age / 60)} min — Thunder may not be advancing`
                    : null };
    }));

    /* Blocks that carry no BIP300/301 commitments are valid and pay miners,
     * so nothing else complains — but no sidechain can be merge-mined into
     * them, which is what stalled Thunder before. */
    checks.push(guard('template_commitments', 'Templates carry sidechain commitments', () => {
        const r = one(d, `SELECT source, cb_op_returns FROM templates
                           ORDER BY id DESC LIMIT 1`);
        if (!r) return { ok: true, unavailable: true, detail: 'no templates recorded yet' };
        const ok = r.source === 'enforcer' && Number(r.cb_op_returns) > 1;
        return { ok, value: Number(r.cb_op_returns),
                 detail: ok ? null
                    : `mining ${r.source} templates with ${r.cb_op_returns} OP_RETURN(s) — no sidechain can merge-mine` };
    }));

    const failing     = checks.filter(c => !c.ok);
    const unavailable = checks.filter(c => c.unavailable);
    return { ok: failing.length === 0, db_ready: true, checks, failing, unavailable };
}

/* ---- snapshot, so page loads never pay for the scan --------------------- */

/* `null` until the first pass completes. Rendered as "checking", never as
 * healthy: silence has to mean "not yet known", or a monitor that never ran
 * looks identical to a pool with nothing wrong. */
let snapshot = null;
let timer = null;

export function currentHealth() {
    return snapshot;
}

export function startHealthMonitor(handle, { intervalMs = 300000 } = {}) {
    const run = () => {
        const startedAt = Date.now();
        try {
            snapshot = { ...health(handle), checked_at: Math.floor(startedAt / 1000),
                         took_ms: Date.now() - startedAt };
        } catch (e) {
            /* The monitor failing is itself a failure worth showing, rather
             * than leaving the previous (possibly stale, possibly green)
             * snapshot in place indefinitely. */
            snapshot = { ok: false, db_ready: false, checks: [], unavailable: [],
                         checked_at: Math.floor(startedAt / 1000),
                         failing: [{ id: 'monitor', label: 'Health monitor',
                                     ok: false, detail: e.message }] };
        }
    };
    run();
    timer = setInterval(run, intervalMs);
    timer.unref?.();     /* never hold the process open */
    return () => { clearInterval(timer); timer = null; };
}
