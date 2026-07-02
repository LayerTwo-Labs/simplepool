PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS workers (
  id              INTEGER PRIMARY KEY AUTOINCREMENT,
  name            TEXT UNIQUE NOT NULL,
  first_seen      INTEGER NOT NULL,
  last_seen       INTEGER NOT NULL,
  payout_address  TEXT
);

CREATE TABLE IF NOT EXISTS shares (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  worker_id   INTEGER NOT NULL REFERENCES workers(id),
  ts          INTEGER NOT NULL,
  difficulty  REAL NOT NULL,
  is_block    INTEGER NOT NULL DEFAULT 0,
  block_hash  TEXT
);
CREATE INDEX IF NOT EXISTS shares_ts_idx ON shares(ts);
CREATE INDEX IF NOT EXISTS shares_worker_ts_idx ON shares(worker_id, ts);

CREATE TABLE IF NOT EXISTS rejects (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  worker_name TEXT,
  ts          INTEGER NOT NULL,
  reason      TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS rejects_ts_idx ON rejects(ts);

CREATE TABLE IF NOT EXISTS blocks_found (
  id              INTEGER PRIMARY KEY AUTOINCREMENT,
  ts              INTEGER NOT NULL,
  height          INTEGER NOT NULL,
  hash            TEXT NOT NULL,
  finder_id       INTEGER REFERENCES workers(id),
  finder_address  TEXT,
  reward_sats     INTEGER,
  fee_sats        INTEGER
);
CREATE INDEX IF NOT EXISTS blocks_found_ts_idx ON blocks_found(ts);

/* Single-row mirror of the upstream bitcoind tip the proxy is currently
 * mining on. Written by the proxy's tip watcher on every successful
 * getblocktemplate poll. Lets the dashboard show "latest block from the
 * node" and "time since last block" without any RPC of its own. */
CREATE TABLE IF NOT EXISTS node_status (
  id              INTEGER PRIMARY KEY CHECK (id = 1),
  tip_height      INTEGER,
  tip_hash        TEXT,
  tip_observed_at INTEGER,  /* unix seconds — when we first saw this tip */
  updated_at      INTEGER   /* unix seconds — last successful poll */
);

/* PPS accrual ledger. One row per worker. The C proxy only INCREMENTs
 * accrued_sats; a separate payout service updates paid_sats after
 * issuing Thunder transactions for (accrued_sats - paid_sats). Empty in
 * solo mode. */
CREATE TABLE IF NOT EXISTS pps_credits (
  worker_id     INTEGER PRIMARY KEY REFERENCES workers(id),
  accrued_sats  INTEGER NOT NULL DEFAULT 0,
  paid_sats     INTEGER NOT NULL DEFAULT 0,
  last_updated  INTEGER NOT NULL
);

/* Ledger of operator-triggered mainchain → Thunder deposits, used by
 * pool_mode=pps-classic. The C proxy does not touch this table; the
 * admin dashboard is the only writer. */
CREATE TABLE IF NOT EXISTS deposits (
  id                INTEGER PRIMARY KEY AUTOINCREMENT,
  ts                INTEGER NOT NULL,          /* unix seconds */
  btc_txid          TEXT    NOT NULL,          /* mainchain deposit tx */
  sats_deposited    INTEGER NOT NULL,
  fee_sats          INTEGER NOT NULL,
  thunder_recipient TEXT    NOT NULL,          /* s9_<base58>_<hex6> */
  ctip_seq_before   INTEGER,
  ctip_seq_after    INTEGER,
  notes             TEXT
);
CREATE INDEX IF NOT EXISTS deposits_ts_idx ON deposits(ts);

/* In-flight payout ledger. The payout worker INSERTs a row before
 * broadcasting a Thunder transaction; on successful broadcast it
 * atomically (in one tx) sets txid, increments pps_credits.paid_sats,
 * and DELETEs the row. The C proxy does not touch this table.
 *
 * Crash semantics:
 *   - Row exists with txid='' → the broadcast may or may not have
 *     happened; needs manual reconciliation. listDue skips workers
 *     that have an in-flight row so we never double-pay.
 *   - Row exists with txid set → the broadcast went out and we crashed
 *     before the DELETE finished. The finalize tx is idempotent (its
 *     paid_sats UPDATE is fenced by the row's existence), so a startup
 *     sweep can finish it.
 */
CREATE TABLE IF NOT EXISTS payouts_in_flight (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  worker_id     INTEGER NOT NULL REFERENCES workers(id),
  sats          INTEGER NOT NULL,
  txid          TEXT NOT NULL DEFAULT '',
  started_at    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS payouts_in_flight_worker_idx ON payouts_in_flight(worker_id);
