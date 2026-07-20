/* Read-write SQLite wrapper for the payout worker.
 *
 * The C proxy is the only writer to pps_credits.accrued_sats — every
 * accepted share INSERTs/UPSERTs with `accrued_sats = accrued_sats + delta`
 * via the writer thread. This worker is the only writer to
 * pps_credits.paid_sats, and only ever after a Thunder transaction has
 * been broadcast successfully.
 *
 * Both writers serialise through WAL with a generous busy_timeout, so
 * brief lock contention during the proxy's batch commit is invisible. */

import Database from 'better-sqlite3';

export function openDb(dbPath) {
    const db = new Database(dbPath, { fileMustExist: true });
    db.pragma('journal_mode = WAL');
    db.pragma('synchronous  = NORMAL');
    db.pragma('busy_timeout = 5000');
    return db;
}

/* Workers with non-zero outstanding balance, joined to the workers table
 * so we have the Thunder address (stored in workers.payout_address by
 * the C proxy on first sight). Filters out:
 *   - balances below minSats
 *   - workers with NO payout_address (shouldn't happen in pps mode but
 *     defensive against legacy rows)
 *   - workers with an in-flight payout row (handled or being handled)
 *
 * The in-flight skip is the safety net for at-most-once payout: a row
 * with txid='' means we crashed between INSERT and broadcast and might
 * have a stuck row, OR between broadcast and finalize. Either way, we
 * don't double-pay; reconciliation is manual (see listStuck). */
export function listDue(db, { minSats, limit }) {
    const rows = db.prepare(`
        SELECT  w.id              AS worker_id,
                w.name            AS worker_name,
                w.payout_address  AS thunder_address,
                c.accrued_sats    AS accrued_sats,
                c.paid_sats       AS paid_sats,
                (c.accrued_sats - c.paid_sats) AS owed_sats
        FROM    pps_credits c
        JOIN    workers     w ON w.id = c.worker_id
        WHERE   (c.accrued_sats - c.paid_sats) >= ?
          AND   w.payout_address IS NOT NULL
          AND   w.payout_address != ''
          AND   NOT EXISTS (
                  SELECT 1 FROM payouts_in_flight p
                   WHERE p.worker_id = w.id
                )
        ORDER BY owed_sats DESC
        LIMIT   ?
    `).all(Number(minSats), limit);
    return rows.map(r => ({ ...r, owed_sats: BigInt(r.owed_sats) }));
}

/* Reserve a payout slot for `workerId`. INSERTed before we touch
 * Thunder; if the worker reboots between INSERT and broadcast the row
 * stays and listDue skips this worker until an operator reconciles.
 * Returns the new row's id. */
export function beginPayout(db, workerId, sats, nowSec) {
    const r = db.prepare(`
        INSERT INTO payouts_in_flight (worker_id, sats, txid, started_at)
        VALUES (?, ?, '', ?)
    `).run(workerId, Number(sats), nowSec);
    return r.lastInsertRowid;
}

/* Atomic finalize after a successful Thunder broadcast:
 *   1. record the txid on the in-flight row (audit trail)
 *   2. increment pps_credits.paid_sats
 *   3. append to the permanent payouts ledger
 *   4. delete the in-flight row
 * All four happen in one SQLite transaction — so on any crash inside
 * this window the state stays consistent (either everything applied or
 * nothing did), and the startup sweep can finish what 3/4 didn't. */
export function finalizePayout(db, rowId, workerId, sats, feeSats, txid, nowSec) {
    db.transaction(() => {
        db.prepare(`
            UPDATE payouts_in_flight SET txid = ? WHERE id = ?
        `).run(txid, rowId);
        db.prepare(`
            UPDATE pps_credits
               SET paid_sats    = paid_sats + ?,
                   last_updated = ?
             WHERE worker_id    = ?
        `).run(Number(sats), nowSec, workerId);
        db.prepare(`
            INSERT INTO payouts (worker_id, sats, fee_sats, txid, paid_at, note)
            VALUES (?, ?, ?, ?, ?, NULL)
        `).run(workerId, Number(sats), Number(feeSats), txid, nowSec);
        db.prepare(`
            DELETE FROM payouts_in_flight WHERE id = ?
        `).run(rowId);
    })();
}

/* Drop an in-flight row that we failed to broadcast — the Thunder RPC
 * threw before any txid was returned, so paid_sats was untouched and
 * the worker is safe to retry next tick. */
export function abortPayout(db, rowId) {
    db.prepare(`DELETE FROM payouts_in_flight WHERE id = ?`).run(rowId);
}

/* Stuck in-flight rows older than `staleAfterSec`. Used at startup to
 * surface anything that needs operator attention; never auto-resolved. */
export function listStuck(db, staleAfterSec, nowSec) {
    return db.prepare(`
        SELECT  p.id, p.worker_id, w.name AS worker_name,
                w.payout_address AS thunder_address,
                p.sats, p.txid, p.started_at
        FROM    payouts_in_flight p
        JOIN    workers           w ON w.id = p.worker_id
        WHERE   p.started_at < ?
        ORDER BY p.started_at ASC
    `).all(nowSec - staleAfterSec);
}
