/* Payout loop.
 *
 * Each tick:
 *   1. SELECT workers from pps_credits where (accrued - paid) >= minSats
 *      AND no in-flight payout row exists for them.
 *   2. Check the Thunder reserve has enough balance to cover the sum
 *      + a fee budget. If not, log and skip the tick.
 *   3. For each due worker, run a three-step at-most-once protocol:
 *        a. beginPayout()      — INSERT in payouts_in_flight (txid='')
 *        b. thunder.transfer() — broadcast; on failure abortPayout() and
 *                                move on (paid_sats untouched, safe to retry)
 *        c. finalizePayout()   — atomic UPDATE(txid) + paid_sats +=
 *                                + DELETE in one SQLite tx
 *
 * Crash semantics:
 *   - Crash between (a) and (b): row stays with txid=''. listDue skips
 *     this worker forever until an operator inspects it and either
 *     reconciles the broadcast (was it sent? if yes finalize manually,
 *     if no delete the row).
 *   - Crash between (b) and (c): same — row exists, broadcast went out,
 *     but paid_sats not yet incremented. listStuck() surfaces it.
 *   - Crash mid-(c): the SQLite tx atomically commits or rolls back.
 *     No partial state.
 *
 * Failure isolation: per-worker payouts are NOT batched into one tx —
 * a bad address or transient RPC error must not block other miners. */

import { listDue, listStuck, beginPayout, finalizePayout, abortPayout } from './db.js';

/* Fee model: flat per-tx fee, configurable later. Thunder is a sidechain
 * with relatively low fees; 100 sats covers a one-input one-output tx
 * comfortably on regtest and should be a sane default for early
 * deployments. Will revisit once mainnet fee dynamics are observable. */
const TX_FEE_SATS = 100n;

export async function runOnce(ctx, log) {
    const { db, thunder, cfg } = ctx;
    const due = listDue(db, { minSats: cfg.minSats, limit: cfg.maxPerTick });
    if (due.length === 0) {
        log.debug?.('payout: no due workers');
        return { attempted: 0, paid: 0, failed: 0 };
    }

    const totalOwed = due.reduce((a, r) => a + r.owed_sats, 0n);
    const totalFees = BigInt(due.length) * TX_FEE_SATS;
    log.info(`payout: ${due.length} due, total owed=${totalOwed} sats, fees~${totalFees}`);

    if (!cfg.dryRun) {
        let bal;
        try {
            bal = await thunder.balance();
        } catch (e) {
            log.warn(`payout: balance() failed (${e.message}); skipping tick`);
            return { attempted: 0, paid: 0, failed: 0 };
        }
        const avail = BigInt(bal.available_sats ?? bal.total_sats ?? 0);
        if (avail < totalOwed + totalFees) {
            log.warn(
                `payout: reserve short — available=${avail} needed=${totalOwed + totalFees}; ` +
                'partial payouts disabled this tick'
            );
            return { attempted: 0, paid: 0, failed: 0, reserve_short: true };
        }
    }

    const now_s = Math.floor(Date.now() / 1000);
    let paid = 0, failed = 0;
    for (const r of due) {
        if (cfg.dryRun) {
            log.info(`payout: DRY ${r.worker_name} -> ${r.thunder_address} ${r.owed_sats} sats`);
            continue;
        }
        const rowId = beginPayout(db, r.worker_id, r.owed_sats, now_s);
        let txid;
        try {
            txid = await thunder.transfer(
                r.thunder_address, r.owed_sats, TX_FEE_SATS);
        } catch (e) {
            /* Broadcast failed cleanly — no txid was issued. Drop the
             * in-flight row so the worker is eligible next tick. */
            abortPayout(db, rowId);
            log.warn(`payout: ${r.worker_name} broadcast failed: ${e.message}`);
            failed++;
            continue;
        }
        try {
            finalizePayout(db, rowId, r.worker_id, r.owed_sats, TX_FEE_SATS, txid, now_s);
            log.info(`payout: ${r.worker_name} -> ${r.thunder_address} ` +
                     `${r.owed_sats} sats txid=${txid}`);
            paid++;
        } catch (e) {
            /* DB error after a successful broadcast. The row now has
             * txid set; listStuck() will surface it on next startup. */
            log.error(`payout: ${r.worker_name} FINALIZE FAILED after ` +
                      `broadcast txid=${txid}: ${e.message}; ` +
                      `paid_sats NOT updated — manual reconciliation required`);
            failed++;
        }
    }
    return { attempted: due.length, paid, failed };
}

/* Called once at startup. Doesn't auto-resolve; logs anything older
 * than `staleAfterSec` so the operator can investigate. */
export function reportStuck(ctx, log, staleAfterSec = 300) {
    const now_s = Math.floor(Date.now() / 1000);
    const rows = listStuck(ctx.db, staleAfterSec, now_s);
    if (rows.length === 0) return;
    log.warn(`payout: ${rows.length} stuck in-flight row(s) (>${staleAfterSec}s old):`);
    for (const r of rows) {
        const age = now_s - r.started_at;
        const state = r.txid ? `broadcast txid=${r.txid}` : 'no txid';
        log.warn(`  worker=${r.worker_name} sats=${r.sats} age=${age}s ${state} ` +
                 `(reconcile: scripts/payout-reconcile.sh ${r.id})`);
    }
}

export function startLoop(ctx, log) {
    let stopped = false;
    let timer = null;

    const tick = async () => {
        if (stopped) return;
        try {
            await runOnce(ctx, log);
        } catch (e) {
            log.error(`payout: unexpected error: ${e.stack || e.message}`);
        }
        if (!stopped) timer = setTimeout(tick, ctx.cfg.intervalMs);
    };

    tick();

    return {
        stop() {
            stopped = true;
            if (timer) clearTimeout(timer);
        },
    };
}
