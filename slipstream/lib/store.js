/* The slipstream service's own database.
 *
 * Three tables, each with one job:
 *
 *   slipstream_submissions  every POST, accepted or not, exactly as it
 *                           arrived. The record of what was asked of the
 *                           pool, kept even when the tx was refused.
 *   slipstream_txs          one row per accepted tx, holding where it stands
 *                           now: pending, in_template, mined, confirmed or
 *                           dropped.
 *   slipstream_events       every status change, append-only, so a row's
 *                           history can be read back rather than inferred.
 *
 * Status meanings:
 *   pending      in the node's mempool, not in the latest template
 *   in_template  in the latest template, i.e. being mined on now
 *   mined        in a block, fewer than the configured confirmations deep
 *   confirmed    that deep
 *   dropped      left the mempool unmined, and the node refused it when it
 *                was sent again: status_reason is the node's reason
 *
 * A tx is relayed once the node accepts it, so another pool may mine it:
 * mined_by_pool says whose block it was.
 */

import Database from 'better-sqlite3';

export const OPEN_STATUSES = ['pending', 'in_template'];

const SCHEMA = `
CREATE TABLE IF NOT EXISTS slipstream_submissions (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  ts           INTEGER NOT NULL,
  submitter    TEXT,
  txid         TEXT,
  raw_hex      TEXT NOT NULL,
  accepted     INTEGER NOT NULL,
  reject_reason TEXT
);
CREATE INDEX IF NOT EXISTS slipstream_submissions_ts_idx ON slipstream_submissions(ts);
CREATE INDEX IF NOT EXISTS slipstream_submissions_txid_idx ON slipstream_submissions(txid);

CREATE TABLE IF NOT EXISTS slipstream_txs (
  txid                 TEXT PRIMARY KEY,
  wtxid                TEXT,
  raw_hex              TEXT NOT NULL,
  vsize                INTEGER NOT NULL,
  weight               INTEGER NOT NULL,
  fee_sats             INTEGER NOT NULL,
  fee_rate             REAL NOT NULL,       -- sat/vB
  required_fee_rate    REAL NOT NULL,       -- what was asked of it, sat/vB
  submitted_at         INTEGER NOT NULL,
  submitted_height     INTEGER,             -- template height at submission
  submitter            TEXT,
  status               TEXT NOT NULL,
  status_reason        TEXT,
  status_at            INTEGER NOT NULL,
  first_in_template_at INTEGER,
  last_in_template_at  INTEGER,
  mined_block_hash     TEXT,
  mined_height         INTEGER,
  mined_at             INTEGER,
  mined_by_pool        INTEGER,             -- 1 ours, 0 not, NULL unknown
  confirmations        INTEGER,
  confirmed_at         INTEGER,
  resubmissions        INTEGER NOT NULL DEFAULT 0  -- rebroadcasts after leaving the mempool
);
CREATE INDEX IF NOT EXISTS slipstream_txs_status_idx ON slipstream_txs(status);
CREATE INDEX IF NOT EXISTS slipstream_txs_submitted_idx ON slipstream_txs(submitted_at);

CREATE TABLE IF NOT EXISTS slipstream_events (
  id      INTEGER PRIMARY KEY AUTOINCREMENT,
  txid    TEXT NOT NULL,
  ts      INTEGER NOT NULL,
  event   TEXT NOT NULL,
  detail  TEXT
);
CREATE INDEX IF NOT EXISTS slipstream_events_txid_idx ON slipstream_events(txid, id);
`;

const now = () => Math.floor(Date.now() / 1000);

export function openStore(path) {
    const db = new Database(path);
    db.pragma('journal_mode = WAL');
    db.pragma('synchronous = NORMAL');
    db.pragma('busy_timeout = 5000');
    db.exec(SCHEMA);
    return new Store(db);
}

export class Store {
    constructor(db) {
        this.db = db;
        this._event = db.prepare(
            'INSERT INTO slipstream_events (txid, ts, event, detail) VALUES (?, ?, ?, ?)');
    }

    logSubmission({ submitter, txid, rawHex, accepted, rejectReason }) {
        this.db.prepare(`
            INSERT INTO slipstream_submissions (ts, submitter, txid, raw_hex, accepted, reject_reason)
            VALUES (?, ?, ?, ?, ?, ?)
        `).run(now(), submitter ?? null, txid ?? null, rawHex, accepted ? 1 : 0, rejectReason ?? null);
    }

    event(txid, event, detail = null) {
        this._event.run(txid, now(), event,
            detail === null || typeof detail === 'string' ? detail : JSON.stringify(detail));
    }

    get(txid) {
        return this.db.prepare('SELECT * FROM slipstream_txs WHERE txid = ?').get(txid) ?? null;
    }

    events(txid) {
        return this.db.prepare(
            'SELECT ts, event, detail FROM slipstream_events WHERE txid = ? ORDER BY id').all(txid);
    }

    insertAccepted(row) {
        const ts = now();
        this.db.transaction(() => {
            this.db.prepare(`
                INSERT INTO slipstream_txs
                  (txid, wtxid, raw_hex, vsize, weight, fee_sats, fee_rate, required_fee_rate,
                   submitted_at, submitted_height, submitter, status, status_at)
                VALUES (@txid, @wtxid, @raw_hex, @vsize, @weight, @fee_sats, @fee_rate,
                        @required_fee_rate, @ts, @submitted_height, @submitter, 'pending', @ts)
            `).run({ ...row, ts });
            this._event.run(row.txid, ts, 'accepted', JSON.stringify({
                fee_sats: row.fee_sats, fee_rate: row.fee_rate,
                required_fee_rate: row.required_fee_rate,
            }));
        })();
    }

    /* Move a row to `status`, recording the change as an event. `fields`
     * are further columns to set alongside. A no-op when nothing changes, so
     * a poll that finds a tx where it already was writes nothing. */
    setStatus(txid, status, { reason = null, fields = {}, detail = null } = {}) {
        const row = this.get(txid);
        if (!row) return false;
        const changed = row.status !== status || (row.status_reason ?? null) !== reason;
        const extra = Object.entries(fields).filter(([k, v]) => row[k] !== v);
        if (!changed && extra.length === 0) return false;
        const ts = now();
        this.db.transaction(() => {
            const sets = ['status = @status', 'status_reason = @reason'];
            if (changed) sets.push('status_at = @ts');
            for (const [k] of extra) sets.push(`${k} = @${k}`);
            this.db.prepare(`UPDATE slipstream_txs SET ${sets.join(', ')} WHERE txid = @txid`)
                .run({ txid, status, reason, ts, ...Object.fromEntries(extra) });
            if (changed) {
                this._event.run(txid, ts, status,
                    detail === null ? reason : JSON.stringify({ reason, ...detail }));
            }
        })();
        return true;
    }

    /* Columns that change with every poll and would drown the event log:
     * set without an event. */
    touch(txid, fields) {
        const keys = Object.keys(fields);
        if (keys.length === 0) return;
        this.db.prepare(
            `UPDATE slipstream_txs SET ${keys.map(k => `${k} = @${k}`).join(', ')} WHERE txid = @txid`,
        ).run({ txid, ...fields });
    }

    byStatus(statuses) {
        const marks = statuses.map(() => '?').join(', ');
        return this.db.prepare(
            `SELECT * FROM slipstream_txs WHERE status IN (${marks}) ORDER BY submitted_at`,
        ).all(...statuses);
    }

    recent({ status = null, limit = 50 } = {}) {
        const cols = `txid, wtxid, vsize, weight, fee_sats, fee_rate, required_fee_rate,
                      submitted_at, status, status_reason, status_at, first_in_template_at,
                      last_in_template_at, mined_block_hash, mined_height, mined_at,
                      mined_by_pool, confirmations, confirmed_at, resubmissions`;
        return status
            ? this.db.prepare(`SELECT ${cols} FROM slipstream_txs WHERE status = ?
                               ORDER BY submitted_at DESC LIMIT ?`).all(status, limit)
            : this.db.prepare(`SELECT ${cols} FROM slipstream_txs
                               ORDER BY submitted_at DESC LIMIT ?`).all(limit);
    }
}
