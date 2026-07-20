-- Ledger of operator-triggered mainchain → Thunder deposits.
-- Populated by the /admin/deposit action (see CLASSIC_PAYOUTS.md).
-- The C proxy does not touch this table; the dashboard is the only writer.
CREATE TABLE IF NOT EXISTS deposits (
    id                INTEGER PRIMARY KEY AUTOINCREMENT,
    ts                INTEGER NOT NULL,          -- unix seconds
    btc_txid          TEXT    NOT NULL,          -- mainchain deposit tx
    sats_deposited    INTEGER NOT NULL,
    fee_sats          INTEGER NOT NULL,
    thunder_recipient TEXT    NOT NULL,          -- s9_<base58>_<hex6>
    ctip_seq_before   INTEGER,                   -- from GetCtip pre-deposit
    ctip_seq_after    INTEGER,                   -- from GetCtip post-deposit
    notes             TEXT
);
CREATE INDEX IF NOT EXISTS deposits_ts_idx ON deposits(ts);
