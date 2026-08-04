/* Payout loop.
 *
 * Each tick:
 *   1. If the previous payout transaction has not confirmed, stop. Thunder
 *      cannot spend the change of an unconfirmed transaction, so there is
 *      nothing to pay from until it is mined (see blockingPayout).
 *   2. SELECT workers from pps_credits where (accrued - paid) >= minSats
 *      AND no in-flight payout row exists for them.
 *   3. Check the Thunder reserve covers the total plus one fee.
 *   4. Pay them ALL in a single transaction:
 *        a. beginBatch()                — INSERT one in-flight row per worker
 *        b. thunder.transferBatchDetailed() — one broadcast for the batch; on
 *                                         failure abortBatch() and retry next
 *                                         tick with paid_sats untouched
 *        c. finalizeBatch()             — atomic, across every worker: txid +
 *                                         paid_sats += + ledger row + DELETE
 *
 * Why one transaction rather than one per worker: Thunder only advances when
 * a mainchain block commits to it, and its wallet cannot spend unconfirmed
 * change — so N transactions cost N sidechain blocks, and the queue drains
 * slower than it fills as soon as the pool has more than a handful of miners.
 * Batching makes throughput independent of miner count.
 *
 * The cost is failure isolation: one bad address fails the whole batch rather
 * than just that worker. That is the right trade here — every recipient is a
 * Thunder address the proxy already validated at authorize time, and a batch
 * that fails leaves nobody credited and nobody stranded, so the next tick
 * simply retries. A persistently bad address surfaces as a repeating failure
 * in tx_attempts with the transaction attached.
 *
 * Crash semantics:
 *   - Crash between (a) and (b): rows stay with txid=''. listDue skips those
 *     workers until an operator reconciles (was it broadcast? if yes finalize
 *     manually, if no delete the rows).
 *   - Crash between (b) and (c): same — rows exist, broadcast went out, but
 *     paid_sats not yet incremented. listStuck() surfaces them.
 *   - Crash mid-(c): the SQLite transaction commits or rolls back as a whole,
 *     across every worker in the batch. No partial credit. */

import { listDue, listStuck, abortPayout, recordTxAttempt, asRawTx,
         lastBroadcastPayout, beginBatch, finalizeBatch, abortBatch } from './db.js';

/* Fee model: flat per-tx fee, configurable later. Thunder is a sidechain
 * with relatively low fees; 100 sats covers a one-input one-output tx
 * comfortably on regtest and should be a sane default for early
 * deployments. Will revisit once mainnet fee dynamics are observable. */
const TX_FEE_SATS = 100n;

/* Only one payout can be in flight at a time.
 *
 * Thunder's wallet picks UTXOs without excluding those already spent by
 * transactions sitting in its own mempool. A second transfer therefore selects
 * the same inputs as the first and is rejected with
 *
 *     mempool error: can't add transaction, utxo double spent
 *
 * and because a transfer consumes every wallet UTXO and returns the remainder
 * as change, that change is unspendable until the first tx is mined. So until
 * it confirms there is nothing to pay from, and every attempt fails
 * identically. Retrying is not just useless, it buries the real state under
 * one failure per due worker per tick.
 *
 * Returns the blocking payout, or null when it is safe to proceed.
 *
 * An unreachable node is treated as blocking: "cannot tell" must not be read
 * as "nothing pending", or we would broadcast against a wallet we cannot see.
 * A txid the node has never heard of is NOT blocking — it was evicted or the
 * node was reset, and nothing is holding the inputs. */
async function blockingPayout(db, thunder, log) {
    const last = lastBroadcastPayout(db);
    if (!last?.txid) return null;

    const st = await thunder.getTransaction(last.txid);
    if (st.error) {
        log.warn(`payout: cannot check ${last.txid.slice(0, 16)}… (${st.error}); ` +
                 'treating as pending');
        return { ...last, reason: 'unreachable' };
    }
    if (!st.known)    return null;   /* evicted or pruned — inputs are free */
    if (st.confirmed) return null;
    return { ...last, reason: 'unconfirmed' };
}

export async function runOnce(ctx, log) {
    const { db, thunder, cfg } = ctx;

    /* Checked before listDue: the answer does not depend on who is owed, and
     * on a blocked pool this is the difference between one log line per tick
     * and one per due worker per tick. */
    if (!cfg.dryRun) {
        const blocked = await blockingPayout(db, thunder, log);
        if (blocked) {
            log.info(
                `payout: waiting on ${blocked.worker_name || 'worker ' + blocked.worker_id} ` +
                `txid=${blocked.txid.slice(0, 16)}… (${blocked.reason}); skipping tick. ` +
                'Thunder must mine a block before its change output is spendable.');
            return { attempted: 0, paid: 0, failed: 0, waiting_on: blocked.txid };
        }
    }

    const due = listDue(db, { minSats: cfg.minSats, limit: cfg.maxPerTick });
    if (due.length === 0) {
        log.debug?.('payout: no due workers');
        return { attempted: 0, paid: 0, failed: 0 };
    }

    const totalOwed = due.reduce((a, r) => a + r.owed_sats, 0n);
    /* One transaction, one fee — not one per recipient. */
    const totalFees = TX_FEE_SATS;
    log.info(`payout: ${due.length} due, total owed=${totalOwed} sats, fee=${totalFees}`);

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

    if (cfg.dryRun) {
        for (const r of due) {
            log.info(`payout: DRY ${r.worker_name} -> ${r.thunder_address} ${r.owed_sats} sats`);
        }
        return { attempted: due.length, paid: 0, failed: 0 };
    }

    /* Everyone due goes out in ONE transaction. Paying them one at a time
     * would cost one sidechain block each, and Thunder only advances when a
     * mainchain block commits to it — so the queue would drain slower than it
     * fills as soon as the pool has more than a handful of miners. */
    const batch = due.map(r => ({
        worker_id: r.worker_id, worker_name: r.worker_name,
        address: r.thunder_address, sats: r.owed_sats,
    }));
    const rowIds = beginBatch(db, batch, now_s);

    let res;
    try {
        res = await thunder.transferBatchDetailed(
            batch.map(b => ({ address: b.address, sats: b.sats })), TX_FEE_SATS);
    } catch (e) {
        /* The whole batch fails together, which is the point: no worker is
         * credited for a transaction that did not go out. */
        abortBatch(db, rowIds);
        recordTxAttempt(db, {
            kind: 'payout', status: 'failed', stage: e.stage || 'unknown',
            rawTx: asRawTx(e.signed || e.unsigned),
            amountSats: totalOwed, feeSats: TX_FEE_SATS,
            destination: batch.length === 1 ? batch[0].address : `${batch.length} recipients`,
            workerId: batch.length === 1 ? batch[0].worker_id : null,
            error: e.message || String(e),
        });
        log.warn(`payout: batch of ${batch.length} ${e.stage || 'transfer'} failed: ${e.message}`);
        return { attempted: due.length, paid: 0, failed: due.length };
    }

    recordTxAttempt(db, {
        kind: 'payout', status: 'broadcast', stage: 'submit',
        txid: res.txid, rawTx: asRawTx(res.signed),
        amountSats: totalOwed, feeSats: TX_FEE_SATS,
        destination: batch.length === 1 ? batch[0].address : `${batch.length} recipients`,
        workerId: batch.length === 1 ? batch[0].worker_id : null,
    });

    try {
        finalizeBatch(db, batch.map((b, i) => ({ ...b, rowId: rowIds[i] })),
                      TX_FEE_SATS, res.txid, now_s);
        log.info(`payout: ${batch.length} worker(s), ${totalOwed} sats, txid=${res.txid}`);
        for (const b of batch) log.info(`  ${b.worker_name} -> ${b.address} ${b.sats} sats`);
        return { attempted: due.length, paid: batch.length, failed: 0, txid: res.txid };
    } catch (e) {
        log.error(`payout: FINALIZE FAILED after broadcast txid=${res.txid}: ${e.message}; ` +
                  `paid_sats NOT updated for ${batch.length} worker(s) — ` +
                  'manual reconciliation required');
        return { attempted: due.length, paid: 0, failed: batch.length };
    }
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
