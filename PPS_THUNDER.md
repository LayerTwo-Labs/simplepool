# `pps-thunder` branch — overview & testing

> **⚠ Architectural finding from end-to-end regtest:** the BIP300
> enforcer does NOT credit coinbase outputs as drivechain deposits — a
> side-by-side test showed a canonical deposit updates the sidechain
> Ctip while a coinbase-shaped deposit does not. The PPS design's
> "every coinbase deposits directly to Thunder" working assumption
> needs revision; see [scripts/regtest/README.md](scripts/regtest/README.md)
> for the empirical evidence and architectural impact discussion. The
> drivechain coinbase builders and PPS plumbing landed on this branch
> are still useful — the shape is correct and the pieces compose — but
> a follow-up "two-step deposit" service is needed to actually move BTC
> into Thunder.

Twelve commits on top of `main` that turn simplepool from a pure solo-
mining proxy into a dual-mode proxy that can also run as a Thunder-
paying PPS pool, with at-most-once payouts, a regtest validation stack
that self-activates BIP300 sidechain #9, a tiny CPU stratum miner that
closes the validation loop, and a statistical fraud audit. SQLite
remains the source of truth; Redis is added as a real-time event
stream alongside it.

```
1ebcea1 Add block-withholding statistical audit (payout/audit.js)
6037a25 Script BIP300 sidechain #9 activation on regtest
e0b849e Add payouts_in_flight ledger for at-most-once payouts
76f8753 Silence WARN-level logs in broadcast test
b0da16f Add BIP300 regtest validation stack
a05a96f Add Thunder payout worker service
571dad4 Support PPS with backend-supplied coinbasetxn (CUSF enforcer)
8783c0b Wire pool_mode=pps end-to-end: Thunder username, drivechain coinbase, PPS accrual
f29ea16 Add Thunder address decoder and drivechain-deposit coinbase builder
b1f3537 Add Redis broadcast for pool events alongside SQLite
```

## What changed

### 1. Redis broadcast (alongside SQLite)
- New module [src/broadcast.c](src/broadcast.c) / [.h](src/broadcast.h).
  Background publisher thread, bounded ring queue, fire-and-forget. SQLite
  remains source of truth; observers never block on Redis I/O.
- Channels published as JSON: `pool:shares`, `pool:rejects`, `pool:blocks`,
  `pool:tip`, `pool:credits`.
- Config: set `redis_url` in `proxy.conf` (empty = module disabled).

### 2. PPS / Thunder pool mode
- New config key `pool_mode = solo | pps` (default `solo`, behaviour
  unchanged). In `pps`:
  - Stratum usernames must be **bare Thunder addresses** — base58 of
    the 20-byte hash. The `s9_<base58>_<hex6>` deposit-format wrapper
    is rejected (Thunder itself doesn't recognize it at the byte level,
    so a miner authorized with it would accrue unpayable PPS balance).
    Validated in [src/thunder.c](src/thunder.c).
  - Every block's coinbase is a **BIP300 drivechain deposit** into the
    configured `pool_thunder_reserve_address` instead of paying the
    miner on BTC. Operator fee stays in BTC.
  - Each accepted share **credits the worker's `pps_credits.accrued_sats`**
    at `pps_sats_per_diff * difficulty`.
- New coinbase builders ([src/coinbase.c](src/coinbase.c)):
  - `coinbase_build_drivechain` — bare build, emits
    `[OP_DRIVECHAIN(9), OP_RETURN(payload), operator BTC fee, witness commit]`.
  - `coinbase_build_drivechain_from_template` — same shape, but rewrites
    a backend-supplied coinbasetxn (CUSF enforcer path).
- New `pps_credits` table — `worker_id`, `accrued_sats`, `paid_sats`,
  `last_updated`. C proxy only writes `accrued_sats`; payout worker only
  writes `paid_sats`.

### 3. Thunder payout worker with at-most-once protocol
- New service under [payout/](payout/) — Node.js, separate process.
- Polls SQLite every 30s for workers with `accrued - paid >= min_sats`,
  calls Thunder's JSON-RPC `transfer(addr, sats, fee)`, then updates
  `paid_sats` on success. Per-miner payouts are not batched — one bad
  address can't block others.
- New `payouts_in_flight` table acts as a write-ahead log: INSERT before
  broadcast, atomic (`txid` write + `paid_sats +=` + DELETE row) in one
  SQLite transaction after. `listDue()` skips any worker with an
  in-flight row → **no double-pay** even if the worker crashes mid-payout.
- `reportStuck()` surfaces rows older than 5 min on startup so an
  operator can reconcile manually; runbook in
  [payout/README.md](payout/README.md).

### 4. Block-withholding audit
- New [payout/audit.js](payout/audit.js) — standalone read-only CLI.
- For each worker over a window:
  `expected_blocks = pool_blocks × (worker_accrued_diff / pool_accrued_diff)`;
  `z = (expected − actual) / sqrt(expected)`.
- Flags suspicious when `expected ≥ 5` and `z ≥ 3` (~1-in-740 false
  positives under honest Poisson sampling).
- No schema changes; runs against the same `data/shares.db`. Human and
  `--json` outputs.

### 5. BIP300 regtest stack
- Under [scripts/regtest/](scripts/regtest/) — `setup.sh`, `start.sh`,
  `stop.sh`, `status.sh`, `activate-thunder.sh`, `validate.sh`,
  `inspect-coinbase.sh`.
- Downloads aarch64-darwin prebuilts of `bitcoind-patched v30.2`,
  `electrs`, and `bip300301_enforcer` and orchestrates them locally.
- `activate-thunder.sh` drives the BIP300 propose/ack flow via the
  enforcer's gRPC at `127.0.0.1:50051` (regtest activation threshold = 6
  votes). Verified end-to-end: proposal at height 1 → activation at
  height 7.

## How to test

### Unit tests (no infra required)

```
make test
```

Runs 7 suites: `share`, `bitcoind`, `stratum`, `store`, `coinbase` (now
12 tests including 4 drivechain ones), `broadcast`, `thunder`. All must
pass. PPS / drivechain coinbase byte layouts are asserted at the byte
level here.

### Solo mode (unchanged from `main`)

Set `pool_mode = solo` (or omit — that's the default) in `proxy.conf`,
run `./build/simplepool proxy.conf`, point a miner at `:3334` with a
BTC address as the username. Same as before.

### PPS mode against the regtest BIP300 stack

```sh
brew install grpcurl       # one-time, needed by activate-thunder.sh

scripts/regtest/setup.sh   # downloads ~50 MB of prebuilt binaries
scripts/regtest/start.sh   # starts bitcoind + electrs + enforcer
scripts/regtest/validate.sh # activates sidechain #9, bootstraps 150 blocks,
                            # probes GBT, prints next-step runbook

# run the pool against the enforcer (see validate.sh output for the
# exact proxy.conf snippet — uses pool_mode=pps, bitcoind_url=
# http://127.0.0.1:18444, etc.)
./build/simplepool /tmp/regtest-proxy.conf

# in another terminal, connect any stratum miner to 127.0.0.1:13334
# with a Thunder-shaped base58 username

# after a block is mined
scripts/regtest/inspect-coinbase.sh

# teardown
scripts/regtest/stop.sh
```

### Redis broadcast

Run a local Redis, set `redis_url = redis://127.0.0.1:6379` in
`proxy.conf`, then in another shell:

```
redis-cli PSUBSCRIBE 'pool:*'
```

Every accepted share / reject / block / tip change / PPS credit will
print as JSON.

### Payout worker (dry run)

```
cd payout && npm install
PAYOUT_DRY_RUN=1 \
PAYOUT_DB_PATH=../data/shares.db \
THUNDER_RPC_URL=http://127.0.0.1:6009 \
THUNDER_FROM_ADDRESS=any \
PAYOUT_INTERVAL_MS=2000 \
node index.js
```

Logs which workers it *would* pay, makes no Thunder RPC calls, doesn't
touch `paid_sats`. Drop `PAYOUT_DRY_RUN=1` once a Thunder node is
reachable to actually pay.

### Block-withholding audit

```
cd payout
PAYOUT_DB_PATH=../data/shares.db node audit.js                      # human-readable
PAYOUT_DB_PATH=../data/shares.db node audit.js --window-hours 168   # week-long window
PAYOUT_DB_PATH=../data/shares.db node audit.js --json               # for cron
```

Safe to run while the proxy is writing — DB is opened read-only and the
queries hit indexed columns only.

## What's deliberately not done

- **Stratum miner choice for the regtest E2E walk-through.** Regtest
  difficulty is `0x207fffff` (trivially low), so any cpuminer or ckpool
  client wired at `127.0.0.1:13334` will find a block in seconds; not
  scripted because the right miner depends on the developer's
  environment.
- **Smart fee model for Thunder payouts.** Flat 100-sat fee for now;
  will need revisiting once Thunder fee dynamics are observable.
- **Confirmation-tracking for Thunder txs.** Thunder's RPC doesn't
  expose per-tx confirmation counts, so we treat a successful broadcast
  as final. Matches how the rest of Thunder tooling works today.
- **Auto-reconciliation of stuck in-flight payout rows.** Operator-driven
  only — we can't safely tell apart "broadcast didn't happen" from
  "broadcast happened, finalize crashed" without a Thunder-side
  mempool/chain lookup. Runbook in [payout/README.md](payout/README.md).

## Files of interest

| | |
| --- | --- |
| [src/broadcast.c](src/broadcast.c)/[.h](src/broadcast.h) | Redis publisher |
| [src/thunder.c](src/thunder.c)/[.h](src/thunder.h) | Thunder address decoder |
| [src/coinbase.c](src/coinbase.c) | `coinbase_build_drivechain[_from_template]` |
| [src/stratum.c](src/stratum.c) | Thunder username auth + drivechain coinbase render |
| [src/store.c](src/store.c) | `pps_credits` + `payouts_in_flight` tables |
| [src/main.c](src/main.c) | OP_RETURN payload precompute + PPS accrual wiring |
| [proxy.conf.example](proxy.conf.example) | All new config keys documented inline |
| [payout/](payout/) | Thunder payout service (at-most-once protocol) |
| [payout/audit.js](payout/audit.js) | Block-withholding statistical audit |
| [scripts/regtest/](scripts/regtest/) | BIP300 regtest stack with sidechain auto-activation |
