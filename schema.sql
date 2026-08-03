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

-- credited_sats is what the share was ACTUALLY credited when it was
-- accepted. The PPS rate is derived per-template and moves with network
-- difficulty, so recomputing historical shares against a current rate
-- misreports them. Audits must sum this column, not re-derive it.
-- 0 in solo mode, and 0 on rows written before the column existed
-- (see pool_meta.credited_from for where it becomes trustworthy).
-- rate_used is the exact rate the proxy multiplied by to get credited_sats.
-- Storing it alongside the result is what makes the credit *verifiable*
-- rather than merely recorded: an auditor can recompute
-- CAST(difficulty * rate_used AS INTEGER) and must get credited_sats back,
-- with no need to know what the rate happened to be at that moment.
-- 0 in solo mode and on rows written before the column existed.
CREATE TABLE IF NOT EXISTS shares (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  worker_id     INTEGER NOT NULL REFERENCES workers(id),
  ts            INTEGER NOT NULL,
  difficulty    REAL NOT NULL,
  is_block      INTEGER NOT NULL DEFAULT 0,
  block_hash    TEXT,
  credited_sats INTEGER NOT NULL DEFAULT 0,
  rate_used     REAL NOT NULL DEFAULT 0
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

/* Single source of truth for what the running proxy is actually paying.
 *
 * The dashboard MUST read the rate from here rather than from its own
 * config or environment — holding the same number in two places is how an
 * audit ends up disagreeing with the ledger it exists to check.
 *
 * rate_source is 'derived' (computed from the live template and fee_bps —
 * the default and recommended setup) or 'override' (operator pinned
 * pps_sats_per_diff, which is taken NET of fee and bypasses fee_bps).
 * effective_fee_bps is what the numbers actually imply, which under an
 * override can differ from the configured fee_bps.
 *
 * credited_from is stamped once, on first write, and marks the point from
 * which shares.credited_sats is populated. */
CREATE TABLE IF NOT EXISTS pool_meta (
  id                  INTEGER PRIMARY KEY CHECK (id = 1),
  pool_mode           TEXT,
  fee_bps             INTEGER,
  rate_source         TEXT,     /* 'derived' | 'override' */
  rate_sats_per_diff  REAL,     /* effective, net of fee; 0 in solo */
  gross_sats_per_diff REAL,     /* fair value before fee */
  effective_fee_bps   REAL,
  network_difficulty  REAL,
  block_value_sats    INTEGER,
  credited_from       INTEGER,  /* unix seconds */
  updated_at          INTEGER   /* unix seconds */
);

/* Append-only log of every distinct PPS rate the proxy has paid at.
 *
 * pool_meta holds one row and is overwritten on every template, so the rate
 * a share was credited at is not recoverable from it after the fact. This
 * table keeps the provenance: what the rate was, and the inputs it was
 * derived from, so an auditor can check the rate itself was fair — not just
 * that the arithmetic was applied consistently (which shares.rate_used
 * already proves on its own).
 *
 * A row is appended only when the tuple actually changes, so on a chain with
 * a quiet mempool this stays small; on a busy one it approaches one row per
 * template. Safe to prune: per-share verification does not depend on it. */
CREATE TABLE IF NOT EXISTS rate_history (
  id                  INTEGER PRIMARY KEY AUTOINCREMENT,
  ts                  INTEGER NOT NULL,  /* unix seconds, when it took effect */
  rate_sats_per_diff  REAL    NOT NULL,  /* net of fee — matches shares.rate_used */
  gross_sats_per_diff REAL    NOT NULL,  /* fair value before fee */
  fee_bps             INTEGER NOT NULL,
  network_difficulty  REAL    NOT NULL,
  block_value_sats    INTEGER NOT NULL,
  rate_source         TEXT    NOT NULL   /* 'derived' | 'override' */
);
CREATE INDEX IF NOT EXISTS rate_history_ts_idx   ON rate_history(ts);
CREATE INDEX IF NOT EXISTS rate_history_rate_idx ON rate_history(rate_sats_per_diff);

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
 * admin dashboard + a helper CLI are the writers. */
CREATE TABLE IF NOT EXISTS deposits (
  id                INTEGER PRIMARY KEY AUTOINCREMENT,
  ts                INTEGER NOT NULL,          /* unix seconds */
  btc_txid          TEXT    NOT NULL,          /* mainchain deposit tx */
  sats_deposited    INTEGER NOT NULL,
  fee_sats          INTEGER NOT NULL,
  thunder_recipient TEXT    NOT NULL,          /* bare base58 Thunder addr */
  ctip_seq_before   INTEGER,
  ctip_seq_after    INTEGER,
  notes             TEXT
);
CREATE INDEX IF NOT EXISTS deposits_ts_idx ON deposits(ts);

/* Permanent ledger of successful miner payouts. One row per settled
 * Thunder transfer — populated by the payout worker inside the same
 * atomic finalize transaction that increments pps_credits.paid_sats
 * and drops the payouts_in_flight row. Only append; never mutate
 * after write (audit trail). Empty in solo mode. */
CREATE TABLE IF NOT EXISTS payouts (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  worker_id    INTEGER NOT NULL REFERENCES workers(id),
  sats         INTEGER NOT NULL,
  fee_sats     INTEGER NOT NULL,
  txid         TEXT    NOT NULL,        /* Thunder tx id (hex) */
  paid_at      INTEGER NOT NULL,        /* unix seconds */
  note         TEXT                     /* 'manual' for hand-driven, else NULL */
);
CREATE INDEX IF NOT EXISTS payouts_worker_ts_idx ON payouts(worker_id, paid_at);
CREATE INDEX IF NOT EXISTS payouts_paid_at_idx   ON payouts(paid_at);

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

/* Every attempt to broadcast a transaction, successful or not. Owned by the
 * dashboard and the payout worker; the C proxy never writes here.
 *
 * `deposits` and `payouts` record what actually happened. A failed broadcast
 * is neither, but it is the thing an operator most needs to see — so it
 * lands here instead, with the raw transaction whenever it can be recovered.
 * Without this a failure left nothing behind but a truncated flash message.
 *
 * raw_tx is the full hex when obtainable. For a deposit that failed at
 * broadcast the enforcer has still signed and stored the tx, so it can be
 * recovered afterwards via ListSidechainDepositTransactions; `stage` records
 * how far the attempt got. */
CREATE TABLE IF NOT EXISTS tx_attempts (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  ts          INTEGER NOT NULL,   /* unix seconds */
  kind        TEXT    NOT NULL,   /* 'deposit' | 'payout' */
  status      TEXT    NOT NULL,   /* 'broadcast' | 'failed' */
  stage       TEXT,               /* step reached when it failed */
  txid        TEXT,
  raw_tx      TEXT,               /* full hex, when recoverable */
  amount_sats INTEGER,
  fee_sats    INTEGER,
  destination TEXT,
  worker_id   INTEGER,            /* payouts only */
  error       TEXT,               /* full, never truncated */
  detail      TEXT                /* JSON: request params */
);
CREATE INDEX IF NOT EXISTS tx_attempts_ts_idx   ON tx_attempts(ts);
CREATE INDEX IF NOT EXISTS tx_attempts_kind_idx ON tx_attempts(kind, ts);
