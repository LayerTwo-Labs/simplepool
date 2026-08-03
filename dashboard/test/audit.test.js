/* Smoke tests for the audit surface.
 *
 * These exist because compiling the EJS templates is not enough: a template
 * can compile fine and still throw at render time, and a lib function can be
 * syntactically valid while calling a helper that is not in scope. Both of
 * those shipped once. Every test here therefore drives the real code path
 * end-to-end — build a DB, query through the real lib functions, render the
 * real template — rather than asserting on shapes.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import Database from 'better-sqlite3';
import ejs from 'ejs';

import * as stats from '../lib/stats.js';
import * as admin from '../lib/admin.js';
import * as fmt from '../lib/fmt.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SCHEMA = path.resolve(__dirname, '../../schema.sql');
const VIEWS  = path.resolve(__dirname, '../views');

const RATE  = 2783.21412641578;   /* derived: 312500000/111157.455 * 0.99 */
const GROSS = 2811.32740041998;

/* A DB shaped like one the proxy has been running against. Returns the raw
 * better-sqlite3 handle plus the lazy {get()} wrapper the dashboard passes
 * around, because different call sites use different ones — which is exactly
 * what broke before. */
function makeDb({ rateSource = 'derived', rate = RATE, feeBps = 100,
                  effectiveFeeBps = 100, shares = 40,
                  /* legacy: emulate a DB written before rate_used existed, so
                   * the credits are present but nothing can be recomputed. */
                  legacy = false, logRate = true } = {}) {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sp-audit-')), 'shares.db');
    const db = new Database(file);
    db.exec(fs.readFileSync(SCHEMA, 'utf8'));

    db.prepare(`INSERT INTO workers (id, name, first_seen, last_seen, payout_address)
                VALUES (1, '3RobWZetukZUXY9kk763AtMyoJtJ.rig1', 1000, 2000, '3RobWZetukZUXY9kk763AtMyoJtJ')`).run();

    /* The proxy appends here on every rate change; a share's rate_used must
     * be findable in it. */
    if (logRate) {
        db.prepare(`INSERT INTO rate_history (ts, rate_sats_per_diff, gross_sats_per_diff,
                        fee_bps, network_difficulty, block_value_sats, rate_source)
                    VALUES (900, ?, ?, ?, 111157.455354832, 312500000, ?)`)
          .run(rate, GROSS, feeBps, rateSource);
    }

    const ins = db.prepare(`INSERT INTO shares (worker_id, ts, difficulty, is_block, block_hash, credited_sats, rate_used)
                            VALUES (1, ?, ?, ?, ?, ?, ?)`);
    let accrued = 0;
    for (let i = 0; i < shares; i++) {
        const diff = 1 + (i % 5);
        const credited = Math.floor(diff * rate);   /* per-share truncation, as the proxy does */
        accrued += credited;
        ins.run(1000 + i, diff, i === 7 ? 1 : 0, 'hash' + i, credited, legacy ? 0 : rate);
    }
    db.prepare(`INSERT INTO pps_credits (worker_id, accrued_sats, paid_sats, last_updated)
                VALUES (1, ?, 0, 2000)`).run(accrued);
    db.prepare(`INSERT INTO pool_meta (id, pool_mode, fee_bps, rate_source,
                    rate_sats_per_diff, gross_sats_per_diff, effective_fee_bps,
                    network_difficulty, block_value_sats, credited_from, updated_at)
                VALUES (1, 'pps-classic', ?, ?, ?, ?, ?, 111157.455354832, 312500000, 900, 2000)`)
      .run(feeBps, rateSource, rate, GROSS, effectiveFeeBps);

    return { db, handle: { get: () => db }, accrued, file };
}

function render(view, locals) {
    return ejs.renderFile(path.join(VIEWS, view),
                          { ...fmt.all, ...locals },
                          { filename: path.join(VIEWS, view) });
}

test('poolMeta accepts both a lazy handle and a raw db', () => {
    const { db, handle } = makeDb();
    /* stats.js passes a resolved db, admin.js passes the wrapper. Both must
     * work — mixing them up threw "unwrap is not defined" at render time. */
    for (const h of [db, handle]) {
        const m = stats.poolMeta(h);
        assert.ok(m, 'poolMeta returned null');
        assert.equal(m.rate_source, 'derived');
        assert.equal(m.fee_bps, 100);
        assert.ok(Math.abs(m.rate_sats_per_diff - RATE) < 1e-6);
        assert.equal(m.accrues, true);
    }
});

test('poolMeta returns null on a DB with no pool_meta row', () => {
    const { db } = makeDb();
    db.prepare('DELETE FROM pool_meta').run();
    assert.equal(stats.poolMeta(db), null);
});

test('audit sums stored credited_sats and matches the ledger exactly', () => {
    const { handle, accrued } = makeDb();
    const audit = admin.workerAudit(handle, 1);
    assert.ok(audit, 'workerAudit returned null');
    assert.equal(audit.totals.accrued_computed, accrued);
    assert.equal(audit.ledger.accrued, accrued);
    /* The whole point: these agree because both come from the same
     * per-share amount, not from re-deriving against a current rate. */
    assert.equal(audit.totals.accrued_computed - audit.ledger.accrued, 0);
    assert.equal(audit.meta.rate_source, 'derived');
});

test('audit does NOT re-derive history from the current rate', () => {
    /* Shares were credited at RATE; the rate then moves (difficulty change).
     * The audit must still report what was actually credited. */
    const { db, handle, accrued } = makeDb();
    db.prepare('UPDATE pool_meta SET rate_sats_per_diff = ?').run(RATE / 2);
    const audit = admin.workerAudit(handle, 1);
    assert.equal(audit.totals.accrued_computed, accrued,
                 'audit changed when the current rate changed — it is re-deriving');
    assert.equal(audit.ledger.accrued, accrued);
});

test('admin-worker template renders (derived)', async () => {
    const { handle } = makeDb();
    const audit = admin.workerAudit(handle, 1);
    audit.payouts = [];
    const html = await render('admin-worker.ejs', { audit });
    assert.match(html, /derived/);
    assert.match(html, /Rate &amp; fee|Rate & fee/);
});

test('admin-worker template renders and flags a drifted override', async () => {
    /* An override whose implied fee has drifted from fee_bps is the exact
     * failure pool_meta exists to expose, so the page must say so. */
    const { handle } = makeDb({
        rateSource: 'override', rate: 1000, feeBps: 100,
        effectiveFeeBps: 6443,          /* 1 - 1000/2811.33 */
    });
    const audit = admin.workerAudit(handle, 1);
    audit.payouts = [];
    const html = await render('admin-worker.ejs', { audit });
    assert.match(html, /override/);
    assert.match(html, /disagrees with the\s+configured/);
});

test('admin-worker template renders on a DB with no pool_meta', async () => {
    const { db, handle } = makeDb();
    db.prepare('DELETE FROM pool_meta').run();
    const audit = admin.workerAudit(handle, 1);
    audit.payouts = [];
    const html = await render('admin-worker.ejs', { audit });
    /* Must degrade to "rate unknown", never crash or invent a rate. */
    assert.match(html, /predates\s+rate publishing/);
});

test('public worker page renders with the pps audit', async () => {
    const { handle } = makeDb();
    const w = stats.worker(handle, '3RobWZetukZUXY9kk763AtMyoJtJ.rig1', 86400);
    assert.ok(w.worker, 'worker not found');
    assert.ok(w.pps_audit, 'pps_audit missing');
    assert.equal(w.pps_audit.accrued, w.pps_audit.accrued_computed);
    const html = await render('worker.ejs', { ...w, name: w.worker.name,
                                              fmtHashrate: stats.fmtHashrate });
    assert.ok(html.length > 0);
});

test('solo-style DB (no credits) does not produce a pps audit', () => {
    const { db, handle } = makeDb();
    db.prepare('DELETE FROM pps_credits').run();
    db.prepare('UPDATE shares SET credited_sats = 0').run();
    const w = stats.worker(handle, '3RobWZetukZUXY9kk763AtMyoJtJ.rig1', 86400);
    assert.equal(w.pps_audit, null);
});

/* ---------------- transaction visibility ---------------- */

import { recordTxAttempt } from '../lib/actions.js';

function attemptsDb() {
    const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sp-tx-')), 'shares.db');
    const db = new Database(file);
    db.exec(fs.readFileSync(SCHEMA, 'utf8'));
    db.prepare(`INSERT INTO workers (id, name, first_seen, last_seen, payout_address)
                VALUES (1, 'rig1', 1000, 2000, '3RobWZetukZUXY9kk763AtMyoJtJ')`).run();
    return { db, handle: { get: () => db } };
}

test('a failed attempt is persisted with its raw transaction and full error', () => {
    const { db, handle } = attemptsDb();
    /* The real drynet3 failure: enforcer signs, bitcoind rejects the
     * OP_DRIVECHAIN output as nonstandard. */
    const err = 'failed to broadcast tx: ErrorObject { code: ServerError(-26), '
              + 'message: "scriptpubkey", data: None }';
    recordTxAttempt(db, {
        kind: 'deposit', status: 'failed', stage: 'broadcast',
        txid: '5d30eee0c5226e2e0a849c87c17dbc99ac2619878ef456af697af8f7539492dd',
        rawTx: '0200000001abcd', amountSats: 100000, feeSats: 1000,
        destination: '3MA4uE5RsmQmJuSNYwGC125NVnVJ',
        error: err, detail: { sidechain_id: 9 },
    });
    const rows = admin.recentTxAttempts(handle, { kind: 'deposit' });
    assert.equal(rows.length, 1);
    assert.equal(rows[0].ok, false);
    assert.equal(rows[0].stage, 'broadcast');
    assert.equal(rows[0].raw_tx, '0200000001abcd');
    /* Untruncated — the reject reason is at the end of the string. */
    assert.equal(rows[0].error, err);
    assert.match(rows[0].error, /scriptpubkey/);
    assert.match(rows[0].detail, /sidechain_id/);
});

test('successful attempts are recorded too, and kinds are separable', () => {
    const { db, handle } = attemptsDb();
    recordTxAttempt(db, { kind: 'deposit', status: 'broadcast', txid: 'aa', amountSats: 5 });
    recordTxAttempt(db, { kind: 'payout',  status: 'broadcast', txid: 'bb', amountSats: 6, workerId: 1 });
    recordTxAttempt(db, { kind: 'payout',  status: 'failed', stage: 'submit', error: 'boom', workerId: 1 });

    assert.equal(admin.recentTxAttempts(handle).length, 3);
    assert.equal(admin.recentTxAttempts(handle, { kind: 'deposit' }).length, 1);
    assert.equal(admin.recentTxAttempts(handle, { kind: 'payout' }).length, 2);
    assert.equal(admin.recentTxAttempts(handle, { failedOnly: true }).length, 1);
    /* Payout rows resolve the worker name for display. */
    const p = admin.recentTxAttempts(handle, { kind: 'payout' }).find(r => !r.ok);
    assert.equal(p.worker_name, 'rig1');
    assert.equal(p.stage, 'submit');
});

test('recording never throws on a DB predating tx_attempts', () => {
    const { db, handle } = attemptsDb();
    db.exec('DROP TABLE tx_attempts');
    assert.equal(recordTxAttempt(db, { kind: 'deposit', status: 'failed' }), null);
    assert.deepEqual(admin.recentTxAttempts(handle), []);
});

test('the deposits ledger INSERT matches the schema', () => {
    /* Regression: the INSERT used ctip_before/ctip_after/note while the
     * schema has ctip_seq_before/ctip_seq_after/notes, so every write threw
     * and no deposit was ever recorded — success or failure. */
    const { db } = attemptsDb();
    db.prepare(`
        INSERT INTO deposits
            (ts, btc_txid, sats_deposited, fee_sats, thunder_recipient,
             ctip_seq_before, ctip_seq_after, notes)
        VALUES (?, ?, ?, ?, ?, NULL, NULL, ?)
    `).run(1, 'txid', 100, 1, 'addr', 'note');
    assert.equal(db.prepare('SELECT count(*) n FROM deposits').get().n, 1);
});

test('deposits and payouts pages render with attempts', async () => {
    const { db, handle } = attemptsDb();
    recordTxAttempt(db, {
        kind: 'deposit', status: 'failed', stage: 'broadcast',
        rawTx: 'deadbeef', amountSats: 100000, destination: 'addr',
        error: 'message: "scriptpubkey"',
    });
    recordTxAttempt(db, {
        kind: 'payout', status: 'broadcast', txid: 'cc',
        rawTx: '{"tx":1}', amountSats: 7, workerId: 1, destination: 'addr2',
    });
    const txAttempts = admin.recentTxAttempts(handle);
    const html = await render('partial/tx-attempts.ejs',
                              { attempts: txAttempts, kindLabel: 'All attempts' });
    assert.match(html, /scriptpubkey/);       /* the error is visible */
    assert.match(html, /deadbeef/);           /* the raw tx is visible */
    assert.match(html, /failed/);
});

test('FULL admin-deposits and admin-payouts pages render', async () => {
    /* Rendering the partial in isolation is not enough — the 500 that shipped
     * in #21 compiled fine and only threw when the real page was assembled.
     * These render the whole view with the locals the router supplies. */
    const { db, handle } = attemptsDb();
    recordTxAttempt(db, {
        kind: 'deposit', status: 'failed', stage: 'broadcast',
        rawTx: 'deadbeef', amountSats: 1, destination: 'a', error: 'scriptpubkey',
    });
    recordTxAttempt(db, {
        kind: 'payout', status: 'broadcast', txid: 'cc',
        amountSats: 2, workerId: 1, destination: 'b',
    });

    const locals = {
        reserve:  { ok: true, balance_sats: 0, available_sats: 0 },
        enforcer: { ok: true, confirmed_sats: 1000, pending_sats: 0 },
        totals:   { accrued: 0, paid: 0, owed: 0, workers: 1 },
        workers:  [],
        inFlight: [],
        payouts:  admin.recentPayouts(handle, 25),
        deposits: admin.recentDeposits(handle, 25),
        blocks:   [],
        txAttempts:            admin.recentTxAttempts(handle),
        reserveAddress:        '3MA4uE5RsmQmJuSNYwGC125NVnVJ',
        reserveSidechainId:    9,
        payoutAdminConfigured: true,
        csrfToken: 'test-token',
        flash:     null,
    };

    for (const view of ['admin-deposits.ejs', 'admin-payouts.ejs']) {
        const html = await render(view, locals);
        assert.ok(html.length > 0, `${view} rendered empty`);
        assert.match(html, /broadcast attempts/i, `${view} missing the attempts table`);
    }
});

/* ---------- verification: the checks, and the failures they must catch ----
 *
 * A verifier that only ever passes is indistinguishable from no verifier, so
 * each of the three checks gets a test that deliberately breaks it. */

test('verification re-derives every credited share from its own stored rate', () => {
    const { db } = makeDb();
    const v = stats.rateVerification(db);
    assert.equal(v.ok, true);
    assert.equal(v.mismatched, 0);
    assert.equal(v.orphaned, 0);
    assert.equal(v.rates_inconsistent, 0);
    assert.equal(v.verifiable, 40);
    assert.equal(v.coverage_pct, 100);
});

test('verification catches a credit that does not match its own rate', () => {
    const { db } = makeDb();
    /* Exactly the tamper the audit exists to detect: the ledger says one
     * thing, the arithmetic says another. */
    db.prepare('UPDATE shares SET credited_sats = credited_sats + 1 WHERE id = 3').run();
    const v = stats.rateVerification(db);
    assert.equal(v.ok, false);
    assert.equal(v.mismatched, 1);
});

test('verification catches a rate that does not follow from its inputs', () => {
    const { db } = makeDb();
    /* Rate applied consistently to every share, but derived wrongly — the
     * per-share check alone cannot see this. */
    db.prepare('UPDATE rate_history SET block_value_sats = 999999999').run();
    const v = stats.rateVerification(db);
    assert.equal(v.ok, false);
    assert.equal(v.rates_inconsistent, 1);
    assert.equal(v.mismatched, 0, 'per-share arithmetic is still self-consistent');
});

test('verification catches shares paid at a rate the pool never published', () => {
    const { db } = makeDb();
    db.prepare('UPDATE rate_history SET rate_sats_per_diff = rate_sats_per_diff * 2').run();
    const v = stats.rateVerification(db);
    assert.equal(v.ok, false);
    assert.equal(v.orphaned, 40);
});

test('an empty rate log does not make every share an orphan', () => {
    /* A pool that has not published a rate yet must report "nothing logged",
     * not "everything is unaccounted for". */
    const { db } = makeDb({ logRate: false });
    const v = stats.rateVerification(db);
    assert.equal(v.rate_rows, 0);
    assert.equal(v.orphaned, 0);
    assert.equal(v.ok, true);
});

test('legacy rows are reported as unverifiable, not as failures', () => {
    const { db } = makeDb({ legacy: true });
    const v = stats.rateVerification(db);
    assert.equal(v.mismatched, 0);
    assert.equal(v.unverifiable, 40);
    assert.equal(v.verifiable, 0);
    assert.equal(v.coverage_pct, 0);
});

test('verification returns null on a DB predating rate_used', () => {
    const { db } = makeDb();
    /* better-sqlite3 has no DROP COLUMN on old SQLite; rebuild without it. */
    db.exec(`
        CREATE TABLE shares_old (
          id INTEGER PRIMARY KEY AUTOINCREMENT, worker_id INTEGER NOT NULL,
          ts INTEGER NOT NULL, difficulty REAL NOT NULL,
          is_block INTEGER NOT NULL DEFAULT 0, block_hash TEXT,
          credited_sats INTEGER NOT NULL DEFAULT 0);
        INSERT INTO shares_old (id, worker_id, ts, difficulty, is_block, block_hash, credited_sats)
          SELECT id, worker_id, ts, difficulty, is_block, block_hash, credited_sats FROM shares;
        DROP TABLE shares;
        ALTER TABLE shares_old RENAME TO shares;
    `);
    assert.equal(stats.rateVerification(db), null);
});

test('verification scopes to one worker', () => {
    const { db } = makeDb();
    db.prepare(`INSERT INTO workers (id, name, first_seen, last_seen, payout_address)
                VALUES (2, 'other.rig', 1000, 2000, 'addr2')`).run();
    db.prepare(`INSERT INTO shares (worker_id, ts, difficulty, is_block, block_hash, credited_sats, rate_used)
                VALUES (2, 1500, 4.0, 0, 'h', 999999, ?)`).run(RATE);   /* wrong on purpose */

    const all = stats.rateVerification(db);
    assert.equal(all.mismatched, 1);

    const mine = stats.rateVerification(db, 1);
    assert.equal(mine.mismatched, 0, 'worker 1 must not inherit worker 2 breakage');
    assert.equal(stats.rateVerification(db, 2).mismatched, 1);
});

test('admin-worker renders the verification section, passing and failing', async () => {
    for (const broken of [false, true]) {
        const { db, handle } = makeDb();
        if (broken) db.prepare('UPDATE shares SET credited_sats = 1 WHERE id = 2').run();
        const audit = admin.workerAudit(handle, 1);
        assert.ok(audit.verification, 'workerAudit must carry the verification');
        const html = await render('admin-worker.ejs', {
            audit, meta: audit.meta, worker: audit.worker,
        });
        assert.match(html, /Verification/);
        assert.match(html, broken ? /Verification failed/ : /re-derives exactly/);
    }
});

test('the public miner page shows the independent check, passing and failing', async () => {
    /* Miners see the same re-derivation the operator does — an audit only the
     * pool can run is not much of an audit. */
    for (const broken of [false, true]) {
        const { db, handle } = makeDb();
        if (broken) db.prepare('UPDATE shares SET credited_sats = 7 WHERE id = 5').run();
        const w = stats.worker(handle, '3RobWZetukZUXY9kk763AtMyoJtJ.rig1', 86400);
        assert.ok(w.pps_audit.verification, 'verification missing from the public audit');
        assert.equal(w.pps_audit.verification.ok, !broken);
        const html = await render('worker.ejs', { ...w, name: w.worker.name,
                                                  fmtHashrate: stats.fmtHashrate });
        assert.match(html, /Independent check/);
        assert.match(html, broken ? /do not match their own stored rate/
                                  : /re-derive exactly/);
    }
});

/* ---------- deposit status ---------------------------------------------
 *
 * The stages are stubbed at the HTTP boundary rather than at the module
 * boundary, so the real enforcerRpc / field-name handling is exercised —
 * that is where the camelCase-vs-snake_case bugs live. */

import * as depstat from '../lib/deposit-status.js';

function withStubbedFetch(handlers, fn) {
    const real = globalThis.fetch;
    globalThis.fetch = async (url, opts) => {
        const u = String(url);
        for (const [match, body] of Object.entries(handlers)) {
            if (u.includes(match)) {
                if (body === 'ERROR') return { ok: false, status: 500, text: async () => '{"code":"unavailable"}' };
                return { ok: true, status: 200, text: async () => JSON.stringify(body),
                         json: async () => body };
            }
        }
        throw new Error(`unstubbed fetch: ${u}`);
    };
    return fn().finally(() => { globalThis.fetch = real; });
}

const TXID = '67c64353b8ee524e761ef06c49e854353693c6ad15255d4491a81b7a17c52bed';

const stubs = ({ confirmed = true, isCtip = true, reserveSats = 0 } = {}) => ({
    'ListSidechainDepositTransactions': {
        transactions: [{
            sidechainNumber: 9,
            tx: {
                txid: { hex: TXID },
                sentSats: '1000000000', feeSats: '100',
                ...(confirmed ? { confirmationInfo: {
                    height: 977589,
                    blockHash: { hex: '00000000000054c40add36402ba525c9b95859647863878a071b2175fb8ea213' },
                } } : {}),
            },
        }],
    },
    'GetChainTip': { blockHeaderInfo: { height: 977600 } },
    'GetCtip':     isCtip ? { ctip: { txid: { hex: TXID }, value: '1000000000' } } : { ctip: null },
    '6009':        { jsonrpc: '2.0', id: 1, result: { total_sats: reserveSats, available_sats: reserveSats } },
});

const DEPOSIT = { id: 1, ts: 1785774125, btc_txid: TXID, sats_deposited: 1000000000,
                  fee_sats: 100, thunder_recipient: '3MA4uE5RsmQmJuSNYwGC125NVnVJ' };

const OPTS = { enforcerGrpcAddr: '127.0.0.1:50051',
               thunderRpcUrl: 'http://127.0.0.1:6009', sidechainId: 9 };

test('a confirmed deposit that is the current Ctip reports stage ctip', async () => {
    await withStubbedFetch(stubs(), async () => {
        const st = await depstat.depositStatuses([DEPOSIT], OPTS);
        assert.equal(st.rows[0].status.stage, 'ctip');
        assert.equal(st.rows[0].status.ok, true);
        assert.equal(st.rows[0].status.confirmations, 12);   /* 977600-977589+1 */
        assert.equal(st.rows[0].status.isCtip, true);
        assert.equal(st.errors.length, 0);
    });
});

test('confirmed-but-not-credited is called out, not reported as done', async () => {
    /* The failure mode this panel exists for: irreversibly on the mainchain,
     * worth nothing on Thunder. */
    await withStubbedFetch(stubs({ reserveSats: 0 }), async () => {
        const st = await depstat.depositStatuses([DEPOSIT], OPTS);
        assert.equal(st.rows[0].status.ok, true);
        assert.equal(st.reserve.total, 0);
        assert.match(depstat.summarise(st), /NOT yet credited on Thunder/);
    });
});

test('a credited reserve does not trigger the stranded warning', async () => {
    await withStubbedFetch(stubs({ reserveSats: 1000000000 }), async () => {
        const st = await depstat.depositStatuses([DEPOSIT], OPTS);
        assert.doesNotMatch(depstat.summarise(st), /NOT yet credited/);
    });
});

test('an unconfirmed deposit reports mempool, not confirmed', async () => {
    await withStubbedFetch(stubs({ confirmed: false }), async () => {
        const st = await depstat.depositStatuses([DEPOSIT], OPTS);
        assert.equal(st.rows[0].status.stage, 'broadcast');
        assert.equal(st.rows[0].status.ok, false);
    });
});

test('a superseded deposit is confirmed but not the Ctip', async () => {
    await withStubbedFetch(stubs({ isCtip: false }), async () => {
        const st = await depstat.depositStatuses([DEPOSIT], OPTS);
        assert.equal(st.rows[0].status.stage, 'confirmed');
        assert.equal(st.rows[0].status.ok, true, 'superseded is normal, not a failure');
        assert.equal(st.rows[0].status.isCtip, false);
    });
});

test('a row with no txid is reported as such, not as unconfirmed', async () => {
    await withStubbedFetch(stubs(), async () => {
        const st = await depstat.depositStatuses(
            [{ ...DEPOSIT, btc_txid: '(pending)' }], OPTS);
        assert.equal(st.rows[0].status.stage, 'no-txid');
    });
});

test('enforcer snake_case field names are handled too', async () => {
    const s = stubs();
    s['ListSidechainDepositTransactions'].transactions[0].tx = {
        txid: { hex: TXID }, sent_sats: '1000000000',
        confirmation_info: { height: 977589, block_hash: { hex: 'ab'.repeat(32) } },
    };
    await withStubbedFetch(s, async () => {
        const st = await depstat.depositStatuses([DEPOSIT], OPTS);
        assert.equal(st.rows[0].status.stage, 'ctip');
        assert.equal(st.rows[0].status.confirmations, 12);
    });
});

test('an enforcer outage degrades to unknown rather than throwing', async () => {
    await withStubbedFetch({ 'ListSidechainDepositTransactions': 'ERROR',
                             'GetChainTip': 'ERROR', 'GetCtip': 'ERROR',
                             '6009': 'ERROR' }, async () => {
        const st = await depstat.depositStatuses([DEPOSIT], OPTS);
        assert.equal(st.rows[0].status.stage, 'unknown');
        assert.ok(st.errors.length > 0, 'errors must be surfaced, not swallowed');
    });
});

test('admin-deposits renders the status column and the stranded warning', async () => {
    const { db } = makeDb();
    db.prepare(`INSERT INTO deposits (ts, btc_txid, sats_deposited, fee_sats,
                    thunder_recipient, notes)
                VALUES (?, ?, ?, ?, ?, '')`)
      .run(DEPOSIT.ts, TXID, DEPOSIT.sats_deposited, DEPOSIT.fee_sats, DEPOSIT.thunder_recipient);

    await withStubbedFetch(stubs({ reserveSats: 0 }), async () => {
        const depositStatus = await depstat.depositStatuses([DEPOSIT], OPTS);
        const html = await render('admin-deposits.ejs', {
            deposits: [DEPOSIT], depositStatus,
            txAttempts: [], reserveAddress: '3MA4uE5RsmQmJuSNYwGC125NVnVJ',
            reserveSidechainId: 9, csrfToken: 'tok', flash: null,
            enforcer: { ok: true, confirmed_sats: 145314677739, pending_sats: 0 },
        });
        assert.match(html, /Check deposit status now/);
        assert.match(html, /current Ctip/);
        assert.match(html.replace(/\s+/g, ' '),
                     /confirmed on the mainchain, but the Thunder reserve is 0/);
        assert.match(html, /block 977,589/);
        assert.match(html.replace(/\s+/g, ' '), /Mainchain tip <strong>977,600<\/strong>/);
    });
});
