/* Payout loop.
 *
 * Each tick:
 *   1. settlePending() — if a batch is outstanding, ask whether it has been
 *      mined. Confirmed: credit it now (finalizeBatch). Still in the mempool:
 *      nudge Thunder to mine and stop, since its change is unspendable until
 *      then and there is nothing to pay from. Undeterminable: stop and shout.
 *   2. SELECT workers from pps_credits where (accrued - paid) >= minSats
 *      AND no in-flight payout row exists for them.
 *   3. Check the Thunder reserve covers the total plus one fee.
 *   4. Pay them ALL in a single transaction:
 *        a. beginBatch()                — INSERT one in-flight row per worker
 *        b. thunder.transferBatchDetailed() — one broadcast for the batch; on
 *                                         failure abortBatch() and retry next
 *                                         tick with paid_sats untouched
 *        c. attachBatchTxid()           — stamp the txid; the rows STAY in
 *                                         flight, and nobody is credited
 *                                         until step 1 of a later tick sees
 *                                         the transaction in a block
 *
 * Why credit on confirmation rather than on broadcast: `paid` is the pool's
 * record of a debt discharged, and a transaction sitting in a mempool has
 * discharged nothing. Crediting at broadcast made `accrued - paid` understate
 * the real liability for as long as settlement took — observed at 4h+ and
 * 265 BTC on drynet3 — and left no path back if the transaction never landed.
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
 *     workers, and listStuck() reports them, until an operator reconciles
 *     (was it broadcast? if yes attach the txid, if no delete the rows). This
 *     is the only genuinely ambiguous state, because a broadcast that
 *     happened is indistinguishable from one that did not.
 *   - Crash between (b) and (c): the same rows with the same remedy, and it
 *     is now a much narrower window — (c) is a local UPDATE, not a credit.
 *   - Crash after (c), before confirmation: nothing special. The rows carry
 *     their txid and the next start settles them normally; this is the
 *     ordinary resting state between a broadcast and a Thunder block.
 *   - Crash mid-finalize: the SQLite transaction commits or rolls back as a
 *     whole, across every worker in the batch. No partial credit. */

import { listDue, listStuck, recordTxAttempt, asRawTx,
         beginBatch, finalizeBatch, abortBatch, attachBatchTxid,
         pendingBatch } from './db.js';

/* Fee model: flat per-tx fee, configurable later. Thunder is a sidechain
 * with relatively low fees; 100 sats covers a one-input one-output tx
 * comfortably on regtest and should be a sane default for early
 * deployments. Will revisit once mainnet fee dynamics are observable. */
const TX_FEE_SATS = 100n;

/* Has `txid` been mined?
 *
 * Thunder gives no single durable answer, so this asks two sources and
 * accepts only POSITIVE evidence from either:
 *
 *   1. get_transaction -> block_hash. Authoritative, but transient: once the
 *      sidechain advances past the block, the txid reads back as `null` —
 *      byte-identical to a txid that never existed. Verified in production:
 *      a payout reported its block_hash while it was the tip and became
 *      indistinguishable from nonexistent one block later.
 *
 *   2. The wallet UTXO set. Each UTXO records the outpoint that created it,
 *      and Thunder only admits confirmed UTXOs — a transfer's change is not
 *      spendable until mined, which is the whole reason payouts serialise.
 *      So an outpoint bearing our txid IS the confirmation, and it is durable:
 *      the change survives until the NEXT payout spends it, and that cannot
 *      happen until this one is finalized.
 *
 * Absence is never read as confirmation, and — just as important — never as
 * eviction. "The node has forgotten it" and "it confirmed a while ago" look
 * the same from here, so inferring eviction and re-queueing the batch would
 * pay it twice. Unknown means unknown; it blocks and asks for a human.
 *
 * Returns 'confirmed' | 'pending' | 'unknown'. */
async function settlementState(thunder, txid, log) {
    const st = await thunder.getTransaction(txid);
    if (st.confirmed) return 'confirmed';

    const w = await thunder.walletUtxos();
    if (w.ok && w.utxos.some(u => u.txid === txid)) return 'confirmed';

    /* Only now can "in the mempool" be trusted as pending: a tx that is both
     * known-unconfirmed and has produced no wallet output is genuinely still
     * settling. */
    if (st.known && !st.error) return 'pending';

    if (st.error) log.warn(`payout: cannot reach Thunder to check ${short(txid)} (${st.error})`);
    else if (!w.ok) log.warn(`payout: cannot read wallet UTXOs to check ${short(txid)} (${w.error})`);
    return 'unknown';
}

const short = txid => `${txid.slice(0, 16)}…`;

/* Settle or wait on the outstanding batch.
 *
 * Only one payout can be in flight at a time. Thunder's wallet picks UTXOs
 * without excluding those already spent by transactions sitting in its own
 * mempool, and a transfer consumes every wallet UTXO and returns the
 * remainder as change — which is unspendable until the first tx is mined. So
 * until it confirms there is nothing to pay from, and every attempt fails
 * identically with
 *
 *     mempool error: can't add transaction, utxo double spent
 *
 * Returns { blocked } — and credits the batch as a side effect when it has
 * confirmed, which is the only place pps_credits.paid_sats ever moves. */
async function settlePending(ctx, log) {
    const { db, thunder } = ctx;
    const pending = pendingBatch(db);
    if (!pending) return { blocked: false };

    const state = await settlementState(thunder, pending.txid, log);

    if (state === 'confirmed') {
        const total = pending.rows.reduce((a, r) => a + r.sats, 0n);
        finalizeBatch(db, pending.rows, TX_FEE_SATS, pending.txid,
                      Math.floor(Date.now() / 1000));
        log.info(`payout: settled ${pending.rows.length} worker(s), ${total} sats, ` +
                 `txid=${short(pending.txid)} confirmed`);
        return { blocked: false, settled: pending.rows.length };
    }

    if (state === 'unknown') {
        /* Deliberately terminal: not credited, not retried, not abandoned.
         * See settlementState — we cannot distinguish confirmed-and-forgotten
         * from never-existed, and the two demand opposite actions. */
        log.error(
            `payout: CANNOT DETERMINE settlement of ${short(pending.txid)} ` +
            `(${pending.rows.length} worker(s), broadcast ` +
            `${Math.floor(Date.now() / 1000) - pending.started_at}s ago). ` +
            'Not crediting and not retrying — payouts are halted until an ' +
            'operator confirms whether this transaction was mined ' +
            `(payout/README.md -> Reconciling by hand). txid=${pending.txid}`);
        return { blocked: true, txid: pending.txid, reason: 'undetermined' };
    }

    await nudgeMine(ctx, log);
    log.info(`payout: waiting on ${short(pending.txid)} (unconfirmed, ` +
             `${pending.rows.length} worker(s)); skipping tick. ` +
             'Thunder must mine a block before this settles.');
    return { blocked: true, txid: pending.txid, reason: 'unconfirmed' };
}

/* Ask Thunder to attempt BMM, at most once per `nudgeIntervalMs`.
 *
 * Thunder advances only when a mainchain block commits to it and nothing
 * schedules that, so a broadcast payout otherwise waits for a human to press
 * the button — measured at over four hours in production, with the whole
 * queue stalled behind it. Nudging here rather than on a timer means it fires
 * exactly when something is waiting to settle: no pending batch, no BMM bid
 * spent on an empty block.
 *
 * Best-effort by construction. A failed nudge must not fail the tick — the
 * batch is already broadcast and safe, and the next tick will try again. */
async function nudgeMine(ctx, log) {
    const { thunder, cfg } = ctx;
    if (!cfg.nudgeMine) return false;
    const now = Date.now();
    if (ctx._lastNudgeMs && now - ctx._lastNudgeMs < cfg.nudgeIntervalMs) return false;
    ctx._lastNudgeMs = now;
    try {
        const r = await thunder.mine();
        log.info('payout: nudged Thunder to mine (a payout is waiting to confirm)' +
                 (r.completed ? '' : ' — BMM request parked, awaiting a mainchain block'));
        return true;
    } catch (e) {
        log.warn(`payout: mine nudge failed (${e.message}); will retry next tick`);
        return false;
    }
}

export async function runOnce(ctx, log) {
    const { db, thunder, cfg } = ctx;

    /* Settle first, pay second — and both before listDue, because the answer
     * does not depend on who is owed, and on a blocked pool this is the
     * difference between one log line per tick and one per due worker.
     *
     * Settling here is what makes `paid_sats` mean confirmed: the previous
     * batch is credited at the moment we can see it in a block, never at the
     * moment it was sent. */
    let settled = 0;
    if (!cfg.dryRun) {
        const st = await settlePending(ctx, log);
        settled = st.settled ?? 0;
        if (st.blocked) {
            return { attempted: 0, paid: 0, failed: 0, settled,
                     waiting_on: st.txid, reason: st.reason };
        }
    }

    const due = listDue(db, { minSats: cfg.minSats, limit: cfg.maxPerTick });
    if (due.length === 0) {
        log.debug?.('payout: no due workers');
        return { attempted: 0, paid: 0, failed: 0, settled };
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
            return { attempted: 0, paid: 0, failed: 0, settled };
        }
        const avail = BigInt(bal.available_sats ?? bal.total_sats ?? 0);
        if (avail < totalOwed + totalFees) {
            log.warn(
                `payout: reserve short — available=${avail} needed=${totalOwed + totalFees}; ` +
                'partial payouts disabled this tick'
            );
            return { attempted: 0, paid: 0, failed: 0, settled, reserve_short: true };
        }
    }

    const now_s = Math.floor(Date.now() / 1000);

    if (cfg.dryRun) {
        for (const r of due) {
            log.info(`payout: DRY ${r.worker_name} -> ${r.thunder_address} ${r.owed_sats} sats`);
        }
        return { attempted: due.length, paid: 0, failed: 0, settled };
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
        return { attempted: due.length, paid: 0, failed: due.length, settled };
    }

    recordTxAttempt(db, {
        kind: 'payout', status: 'broadcast', stage: 'submit',
        txid: res.txid, rawTx: asRawTx(res.signed),
        amountSats: totalOwed, feeSats: TX_FEE_SATS,
        destination: batch.length === 1 ? batch[0].address : `${batch.length} recipients`,
        workerId: batch.length === 1 ? batch[0].worker_id : null,
    });

    /* Broadcast is not settlement: stamp the txid and leave the rows in
     * flight. Nobody is credited until a later tick sees the transaction in a
     * block. If this throws, the rows keep txid='' — the ambiguous case that
     * listStuck reports and only an operator can resolve, exactly as before. */
    try {
        attachBatchTxid(db, rowIds, res.txid);
        log.info(`payout: broadcast ${batch.length} worker(s), ${totalOwed} sats, ` +
                 `txid=${res.txid} — awaiting confirmation`);
        for (const b of batch) log.info(`  ${b.worker_name} -> ${b.address} ${b.sats} sats`);
        await nudgeMine(ctx, log);
        return { attempted: due.length, paid: 0, broadcast: batch.length,
                 failed: 0, settled, txid: res.txid };
    } catch (e) {
        log.error(`payout: could not record txid=${res.txid} after broadcast: ${e.message}; ` +
                  `${batch.length} in-flight row(s) left without a txid — ` +
                  'manual reconciliation required — see payout/README.md');
        return { attempted: due.length, paid: 0, failed: batch.length, settled };
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
                 `(reconcile: payout/README.md, row id=${r.id})`);
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
