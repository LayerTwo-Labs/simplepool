# simplepool-payout

Thunder payout worker. Drains `pps_credits.accrued_sats - paid_sats` from
the shared SQLite database by issuing Thunder transactions on a cadence,
then writes back `paid_sats`.

Lives outside the C proxy so the hot path (block-template / share
acceptance) is never blocked on Thunder RPC latency. The C proxy is the
only writer of `accrued_sats`; this worker is the only writer of
`paid_sats`. SQLite WAL + a 5-second busy timeout keep them out of each
other's way.

## Run

```
PAYOUT_DB_PATH=../data/shares.db \
THUNDER_RPC_URL=http://127.0.0.1:6009 \
THUNDER_FROM_ADDRESS=<pool base58 thunder address> \
node index.js
```

Dry run — log what would be paid, skip Thunder RPC and DB writes:

```
PAYOUT_DRY_RUN=1 PAYOUT_DB_PATH=../data/shares.db \
  THUNDER_RPC_URL=http://127.0.0.1:6009 \
  THUNDER_FROM_ADDRESS=any \
  node index.js
```

## Config (environment variables)

| var | required | default | meaning |
| --- | --- | --- | --- |
| `PAYOUT_DB_PATH` | yes | — | path to `data/shares.db` (writable) |
| `THUNDER_RPC_URL` | yes | — | Thunder JSON-RPC endpoint, e.g. `http://127.0.0.1:6009` |
| `THUNDER_FROM_ADDRESS` | yes | — | pool reserve address; must equal `pool_thunder_reserve_address` in `proxy.conf` |
| `THUNDER_RPC_USER` / `THUNDER_RPC_PASS` | no | — | basic-auth if your Thunder node has it (default Thunder build has none) |
| `PAYOUT_INTERVAL_MS` | no | 30000 | how often to scan |
| `PAYOUT_MIN_SATS` | no | 10000 | skip workers below this owed balance |
| `PAYOUT_MAX_PER_TICK` | no | 50 | cap workers paid per scan |
| `PAYOUT_DRY_RUN` | no | — | `1` = log only |
| `PAYOUT_DEBUG` | no | — | `1` = verbose |

## What it does each tick

1. `SELECT … FROM pps_credits JOIN workers WHERE accrued - paid >= min`
2. `thunder.balance()` — bail this tick if the reserve is short
3. For each due worker: `thunder.transfer(addr, owed, fee)`; on success,
   `UPDATE pps_credits SET paid_sats = paid_sats + owed`

Payouts are **not batched** into a single tx — one bad address or RPC
error must not block other miners.

## At-most-once payout protocol

Every payout flows through three steps so a crash anywhere in the
middle can't double-pay:

1. `INSERT INTO payouts_in_flight (worker_id, sats, txid='')` — reserve
   the slot before Thunder is touched. `listDue()` skips any worker
   with an in-flight row.
2. `thunder.transfer(addr, sats, fee)` — broadcast. On clean failure
   (RPC error before a txid is returned), the row is DELETEd and the
   worker is eligible next tick.
3. In ONE SQLite transaction: write the txid onto the in-flight row,
   `paid_sats += sats`, DELETE the in-flight row.

Crash semantics:

| crash point | row state | action |
| --- | --- | --- |
| after (1), before (2) | txid='' | manual: did broadcast happen? unlikely. delete row. |
| after (2), before (3) | txid='<id>' | manual: tx is on Thunder — finalize by hand. |
| inside (3) | atomic — either fully applied or fully rolled back | nothing |

On startup the worker calls `reportStuck()` which logs every in-flight
row older than 5 minutes. Operator-driven reconciliation only; we
never silently auto-finalize because we cannot safely distinguish
"broadcast didn't happen" from "broadcast happened, finalize crashed"
without Thunder-side mempool/chain lookup.

To reconcile a stuck row by hand once you've confirmed via the Thunder
node whether the tx is live:

```
# if the tx exists on Thunder: finalize manually
sqlite3 data/shares.db "
  BEGIN;
  UPDATE pps_credits   SET paid_sats = paid_sats + (SELECT sats FROM payouts_in_flight WHERE id = <id>)
                       WHERE worker_id = (SELECT worker_id FROM payouts_in_flight WHERE id = <id>);
  DELETE FROM payouts_in_flight WHERE id = <id>;
  COMMIT;
"

# if the tx never made it: just delete the row
sqlite3 data/shares.db "DELETE FROM payouts_in_flight WHERE id = <id>;"
```

## Block-withholding audit (`audit.js`)

PPS-specific fraud check. A worker can submit valid shares to collect
PPS payouts and then quietly *not submit* the ones that would be
blocks, keeping the on-chain reward off-pool. Detection is statistical:
each worker's accrued share difficulty as a fraction of the pool's
total predicts what fraction of blocks they should have found.

```
PAYOUT_DB_PATH=../data/shares.db node audit.js
PAYOUT_DB_PATH=../data/shares.db node audit.js --window-hours 168
PAYOUT_DB_PATH=../data/shares.db node audit.js --json    # for cron / slack
```

For each worker over the window:
- **expected_blocks** = `pool_blocks * (worker_accrued / pool_accrued)`
- **actual_blocks**   = blocks they actually found
- **z**               = `(expected - actual) / sqrt(expected)`

A worker is flagged `suspicious` when:
- `expected_blocks >= 5` (below this, randomness dominates), AND
- `z >= 3` (≈1-in-740 false-positive rate under honest mining)

Run on a cron and pipe the `--json` output to your alert sink of choice.
The audit reads the DB only — safe to run while the proxy is writing.

## Known gaps (deliberate)

- **Flat 100-sat fee.** Will need a smarter fee model once Thunder
  fee dynamics are observable. Currently hardcoded in `lib/payout.js`.
- **No confirmation tracking.** Thunder's RPC doesn't expose per-tx
  confirmation counts; we treat a successful broadcast as final.
  That's how the rest of Thunder tooling works today.
- **Manual reconciliation.** Crashes between broadcast and finalize
  need an operator. The alternative (auto-reconcile via Thunder
  mempool/chain lookup) is fragile without a getrawtransaction-style
  endpoint and is left as a follow-up.
