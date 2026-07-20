# `pps-thunder-classic` — traditional coinbase + operator-driven deposits

## Why this branch exists

The `pps-thunder` design embeds a BIP300 drivechain deposit in every
coinbase, aiming to make the pool never custody BTC. End-to-end
validation on regtest AND on the live forknet server proved that the
LayerTwo-Labs enforcer **does not credit coinbase outputs as
drivechain deposits** — the block is accepted into the chain but the
sidechain Ctip never moves. The rule requires the deposit tx to spend
real, mature, spendable UTXOs; a coinbase does not qualify. This is
consensus-level and unlikely to change.

This branch flips the model to the pattern all real drivechain mining
pools converge on:

1. **Coinbase pays the pool's BTC wallet** — a normal solo-style
   output. Pool now does briefly custody BTC (a design tradeoff, but
   the only path that actually works).
2. **Operator (via the admin dashboard) triggers batched deposits to
   Thunder.** Each deposit is a real `CreateDepositTransaction` that
   spends accumulated pool UTXOs → OP_DRIVECHAIN + OP_RETURN. This DOES
   credit the Ctip on Thunder.
3. **Payout worker (already in the code) drains the Thunder reserve to
   miners.** No change from `pps-thunder` — same
   `pps_credits.accrued_sats - paid_sats` sweep, same at-most-once
   protocol.

Everything upstream of the deposit step stays identical to
`pps-thunder`: same stratum-username-is-a-Thunder-address, same PPS
accrual math, same in-flight ledger, same audit tooling.

## Concrete code delta from `pps-thunder`

### 1. New pool mode value

Add a third value to `pool_mode` in `src/config.h` /
`src/config.c`:

- `solo`  — unchanged; miners paid direct in coinbase (per-miner
  address as the coinbase spendable output).
- `pps`   — the drivechain-in-coinbase build (does not credit Thunder;
  useful only as a shape validator).
- **`pps-classic`** — new. Coinbase pays a single `pool_btc_address`
  (P2WPKH) for the full net-of-operator-fee reward. PPS accrual math
  and stratum username validation are the same as `pps`.

In `pps-classic` mode the stratum server:
- Validates usernames as Thunder addresses (as `pps` does now).
- Renders one coinbase, identical for every miner, paying
  `pool_btc_address` for `value - fee` and `operator_address` for
  `fee`. No OP_DRIVECHAIN output; no OP_RETURN destination.
- Accrues `difficulty × pps_sats_per_diff` to each worker's
  `pps_credits.accrued_sats` (unchanged).

### 2. New config keys

Add to `proxy.conf`:

```
pool_mode = pps-classic

# Where mined BTC lands. Should be a wallet the operator controls
# and that has enough age/maturity for later deposit-tx use.
pool_btc_address = bc1q...

# Everything else — pps_sats_per_diff, operator_address, fee_bps —
# behaves exactly as in pps mode. pool_thunder_reserve_address is
# ignored in this mode (deposits go via the admin dashboard, not
# the coinbase).
```

### 3. New coinbase builder

`src/coinbase.c`: `coinbase_build_split` already emits
`[pool_btc_p2wpkh, operator_fee, witness_commit]` — that IS the
classic-mode layout. No new builder required. Just call it with
`miner_address = pool_btc_address` in `stratum.c`.

The `_drivechain*` builders stay in the codebase (they still power
`pool_mode = pps` for shape validation), but aren't reached in
classic mode.

### 4. New database table

`deposits` — one row per operator-triggered Thunder deposit:

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

### 5. New admin controls

Extend `dashboard/views/admin.ejs` and `dashboard/server.js` with a
new **"Deposit to Thunder"** card:

- Shows: `pool_btc_address` current spendable balance (via bitcoind
  `getreceivedbyaddress` or a lightweight scan), amount already
  waiting on-chain vs already deposited (from `deposits` table).
- **POST /admin/deposit** — form fields: `amount_sats`, `fee_sats`.
  Server calls the enforcer's gRPC
  `WalletService/CreateDepositTransaction` with the pool's Thunder
  reserve address as the destination. On success, `INSERT INTO
  deposits` + refresh reserve balance.
- All controls require the same basic auth as the read-only admin
  view; CSRF protection via a per-session token (or an
  `Origin`-header check for simplicity).

### 6. New operator-facing docs

- Update `PPS_THUNDER.md` with a subsection pointing at this file for
  the classic-mode alternative.
- Update `payout/README.md` — noting that in classic mode the reserve
  is filled manually via the admin, not automatically via coinbase.

## Migration story on the live server

None required. `pps-thunder-classic` is a config change and a coinbase-
builder swap on the same binary:

```sh
# on the forknet box, once this branch merges to main deployment:
sed -i 's/^pool_mode = pps/pool_mode = pps-classic/' proxy.conf
echo 'pool_btc_address = bc1q...' >> proxy.conf   # pool's BTC wallet
systemctl restart simplepool.service
```

Existing `pps_credits` accruals carry over. In-flight ledger keeps
working. Miners don't reconnect (stratum username is still a Thunder
address).

## What's NOT in this branch yet

This branch currently contains only:
- **this doc**,
- a placeholder in `src/config.h` marked TODO for the new
  `pool_mode` value,
- a placeholder `deploy/schema/deposits.sql` with the deposits table
  SQL.

The actual C changes (config parser, stratum coinbase-render branch),
the SQL wiring in `src/store.c`, the admin dashboard controls, and
the gRPC client for the enforcer's `CreateDepositTransaction` are
open work. When you're ready to build it, this doc is the blueprint.

## Why do the design first

Because the interesting decisions are all in the deposit-flow shape,
not the code. Locking in:

- **Manual vs auto deposits.** This design says manual (operator
  clicks a button per deposit). An auto-batching worker is a later
  improvement — no schema change required, just a new service that
  posts to `/admin/deposit`.
- **One pool BTC address vs many.** One is simpler and matches how
  drivechain-launcher wallets typically hold funds. Migrating to a
  rolling set of addresses is a follow-up.
- **How much precision on the deposit fee.** Locked to
  `enforcer.WalletService.CreateDepositTransaction`'s `fee_sats`
  field. Operator eyeballs current fee market and picks a number.
