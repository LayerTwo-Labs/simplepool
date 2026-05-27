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
