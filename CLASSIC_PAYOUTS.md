# `pps-classic` — traditional coinbase + operator-driven deposits

This is the design behind `pool_mode = pps-classic`, the pool's
Thunder-paying PPS mode. It is implemented and running; this doc
explains the shape and why it looks the way it does.

## Why not deposit straight from the coinbase

The original design (`pool_mode = pps`, since removed) embedded a BIP300
drivechain deposit in every coinbase, so the pool would never custody
BTC. End-to-end validation on regtest **and** on the live forknet server
proved that the LayerTwo-Labs enforcer **does not credit coinbase
outputs as drivechain deposits** — the block is accepted into the chain
but the sidechain Ctip never moves. A side-by-side test:

| deposit shape | Ctip moved? |
| --- | --- |
| canonical `CreateDepositTransaction` (spends mature UTXOs) | yes |
| simplepool coinbase, `OP_DRIVECHAIN(9)` output | **no** |

The rule requires the deposit tx to spend real, mature, spendable
UTXOs; a coinbase does not qualify. This is consensus-level and unlikely
to change, so that mode stranded the block reward and was deleted along
with its coinbase builders. `pps-classic` is what all real drivechain
mining pools converge on instead.

## The flow

1. **Coinbase pays the pool's BTC wallet** — a normal solo-style output
   to `pool_btc_address` for the full net-of-operator-fee reward. The
   pool does briefly custody BTC (a design tradeoff, but the only path
   that actually works). The operator fee stays in BTC, paid to
   `operator_address` out of the same coinbase.
2. **Operator triggers batched deposits to Thunder** via the admin
   dashboard. Each deposit is a real `CreateDepositTransaction` that
   spends accumulated pool UTXOs → OP_DRIVECHAIN + OP_RETURN. This DOES
   credit the Ctip on Thunder.
3. **The payout worker drains the Thunder reserve to miners** — the
   `pps_credits.accrued_sats - paid_sats` sweep under [payout/](payout/),
   with an at-most-once protocol backed by the `payouts_in_flight`
   write-ahead table.

Stratum usernames are **bare base58 Thunder addresses** — the
`s9_<base58>_<hex6>` deposit-format wrapper is rejected, because Thunder
doesn't recognize it at the byte level and a miner authorized with it
would accrue unpayable PPS balance. Validated in
[src/thunder.c](src/thunder.c).

Each accepted share credits the worker's `pps_credits.accrued_sats` at
`pps_sats_per_diff * difficulty`, truncated to whole sats.

## Config

```
pool_mode = pps-classic

# Where mined BTC lands. Should be a wallet the operator controls
# and that has enough age/maturity for later deposit-tx use.
pool_btc_address = bc1q...

# Sats credited per unit of share difficulty.
pps_sats_per_diff = 1000

# operator_address and fee_bps behave exactly as in solo mode.
```

The pool's Thunder reserve address is **not** a proxy config key — the
coinbase never touches Thunder. It is set on the dashboard
(`POOL_THUNDER_RESERVE_ADDRESS`) and on the payout worker
(`THUNDER_FROM_ADDRESS`), which are the two components that actually
speak to Thunder.

## Coinbase builder

`src/coinbase.c`: `coinbase_build_split` emits
`[pool_btc_p2wpkh, operator_fee, witness_commit]` — that IS the
classic-mode layout, called with `miner_address = pool_btc_address` in
`stratum.c`. When the backend dictates the coinbase (the CUSF enforcer
path), `coinbase_build_from_template` rewrites the spendable output the
same way while preserving the BIP301 commitment outputs byte-for-byte.

## Database

- `pps_credits` — one row per worker: `worker_id`, `accrued_sats`,
  `paid_sats`, `last_updated`. The C proxy only INCREMENTs
  `accrued_sats`; the payout worker only writes `paid_sats`.
- `payouts_in_flight` — write-ahead log for the at-most-once payout
  protocol. INSERT before broadcast; one atomic transaction after
  (`txid` write + `paid_sats +=` + DELETE row). `listDue()` skips any
  worker with an in-flight row, so a crash mid-payout can't double-pay.
  Runbook in [payout/README.md](payout/README.md).
- `deposits` — one row per operator-triggered Thunder deposit:

```sql
CREATE TABLE IF NOT EXISTS deposits (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  ts            INTEGER NOT NULL,           -- unix seconds
  btc_txid      TEXT    NOT NULL,           -- mainchain deposit tx
  sats_deposited INTEGER NOT NULL,
  fee_sats      INTEGER NOT NULL,
  thunder_recipient TEXT NOT NULL,          -- deposit-format address
  ctip_seq_before INTEGER,                  -- for audit trail
  ctip_seq_after  INTEGER,
  notes         TEXT
);
CREATE INDEX IF NOT EXISTS deposits_ts_idx ON deposits(ts);
```

## Admin controls

The **"Deposit to Thunder"** card on `/admin/deposits`:

- Shows the pool wallet's spendable balance, what's waiting on-chain vs
  already deposited (from the `deposits` table), and the current Thunder
  reserve balance.
- **POST /admin/deposit** — fields `amount_sats`, `fee_sats`. Calls the
  enforcer's gRPC `WalletService/CreateDepositTransaction` with the
  pool's Thunder reserve address as the destination. On success,
  `INSERT INTO deposits` + refresh reserve balance.
- Same basic auth as the read-only admin view, plus an `Origin`-header
  check.

## Block-withholding audit

[payout/audit.js](payout/audit.js) — standalone read-only CLI. For each
worker over a window:
`expected_blocks = pool_blocks × (worker_accrued_diff / pool_accrued_diff)`;
`z = (expected − actual) / sqrt(expected)`. Flags suspicious when
`expected ≥ 5` and `z ≥ 3` (~1-in-740 false positives under honest
Poisson sampling). No schema changes; safe to run while the proxy is
writing.

## Locked-in decisions

- **Manual vs auto deposits.** Manual — the operator clicks a button per
  deposit. An auto-batching worker is a later improvement; no schema
  change required, just a new service that posts to `/admin/deposit`.
- **One pool BTC address vs many.** One is simpler and matches how
  drivechain-launcher wallets typically hold funds. A rolling set of
  addresses is a follow-up.
- **Deposit fee precision.** Locked to
  `enforcer.WalletService.CreateDepositTransaction`'s `fee_sats` field.
  The operator eyeballs the current fee market and picks a number.
- **Thunder payout fee.** Flat 100 sats for now; needs revisiting once
  Thunder fee dynamics are observable.
- **Confirmation tracking.** Thunder's RPC doesn't expose per-tx
  confirmation counts, so a successful broadcast is treated as final.
  Matches the rest of the Thunder tooling today.
- **Stuck in-flight rows** are reconciled by the operator, not
  automatically — we can't safely tell "broadcast didn't happen" from
  "broadcast happened, finalize crashed" without a Thunder-side
  mempool/chain lookup.
