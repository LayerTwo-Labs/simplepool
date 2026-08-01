# BIP300 regtest validation stack

Local stack for validating simplepool's coinbase shape against the
canonical LayerTwo-Labs enforcer, and for reproducing the finding that
killed the coinbase-as-deposit design (see below).

## Stack

```
                       ┌──────────────┐  GBT 18444
                       │  enforcer    │◀────── simplepool (pps-classic)
                       │              │
              ┌────────┤              ├────────┐
              │  ZMQ   │              │ gRPC   │
              │ 29010  └──────────────┘ 50051  │
              ▼              │                 ▼
        ┌──────────┐         │           (events,
        │ bitcoind │◀────────┘   sidechain CRUD)
        │ regtest  │  RPC 18443
        │ patched  │
        └──────────┘
```

- **bitcoind-patched** (v30.2): BIP300/301-aware Bitcoin Core fork, regtest mode.
- **bip300301_enforcer**: validates BIP300 deposits, serves the
  `getblocktemplate` simplepool talks to. Its wallet runs with
  `--wallet-sync-source=disabled` — it stays in sync purely from
  incoming blocks, which is complete on a from-genesis regtest chain,
  so no electrs/Electrum server is needed.
- **thunder** (L2-S9): the actual sidechain node, connected to the
  enforcer via gRPC on 50051, RPC on 6009 (matches what
  `payout/lib/thunder.js` expects out of the box).

Env knobs (all scripts): `REGTEST_DIR` relocates chain state, logs and
pidfiles away from `.regtest/`; `REGTEST_BIN_DIR` relocates the binary
cache (downloaded zips + extracted binaries) independently of the data
(default: `$REGTEST_DIR/bin`), so data can be wiped or moved without
re-downloading; `REGTEST_SKIP_THUNDER=1` skips downloading/starting
thunder; `REGTEST_WALLETLESS=1` starts the enforcer with no wallet at
all (template rewards go to a fixed regtest address). CI uses all of
these. Sidechain activation works walletless: the block producer's
persisted ack policy (`SetAckAllProposals`) makes every enforcer-built
coinbase carry the M1/M2 commitments, and blocks are mined via
`MiningService/GenerateToAddress` (enforcer PR #477 — the
enforcer prebuilt must be new enough to have it).

## Quickstart

```
scripts/regtest/setup.sh             # download prebuilts + write configs
scripts/regtest/start.sh             # bring up bitcoind, enforcer, thunder
scripts/regtest/status.sh            # ps-style summary
scripts/regtest/activate-thunder.sh  # propose + mine sidechain #9 until active
scripts/regtest/thunder-init.sh      # generate Thunder wallet mnemonic + address
scripts/regtest/validate.sh          # activate, mine 150, probe GBT, print runbook
scripts/regtest/inspect-coinbase.sh  # after mining: parse tip's coinbase
scripts/regtest/stop.sh
```

`activate-thunder.sh` and `thunder-init.sh` are both idempotent —
re-running once the state is set up is a no-op.

Everything lives under `.regtest/` (gitignored): binaries in
`.regtest/bin/`, chain state in `.regtest/data/`, logs in
`.regtest/logs/`, pidfiles in `.regtest/run/`.

## What's verified today

Running `start.sh` brings up the full stack cleanly on aarch64-darwin
(macOS Apple Silicon):

- bitcoind-patched v30.2 listens on `127.0.0.1:18443`.
- electrs indexes the regtest chain on `127.0.0.1:60401`.
- enforcer syncs to tip in ~5s and serves `getblocktemplate` on
  `127.0.0.1:18444`.

`validate.sh` mines 150 blocks to a P2WPKH miner address, calls GBT
on the enforcer, and prints the next-step runbook.

## End-to-end validation status

**All infra steps verified.** A tiny CPU stratum miner
([`cpuminer.js`](cpuminer.js)) connects to simplepool running against
the enforcer, finds a regtest block in seconds, and submits it. The
block is accepted into the regtest chain by bitcoind-patched and the
enforcer, and `inspect-coinbase.sh` confirms the coinbase layout.

When this stack was first built the pool ran a `pool_mode=pps` build
that put an `OP_DRIVECHAIN(9)` output straight in the coinbase.
bitcoind classified the output as `"type": "drivechain"` and the
OP_RETURN destination immediately followed — the shape was right, but
see the finding below. That mode has since been removed; the pool now
runs `pool_mode=pps-classic` here.

**Critical finding from running the loop:** the enforcer DOES NOT
credit coinbase outputs as drivechain deposits. A side-by-side test:

| deposit source | enforcer Ctip update |
| --- | --- |
| simplepool coinbase, OP_DRIVECHAIN(9) value 49.5 BTC | NO (Ctip stays empty) |
| `WalletService/CreateDepositTransaction` 1 BTC | YES (Ctip → 100,000,000 sats) |

Both blocks are accepted into the chain. The difference is at the
consensus-level deposit-recognition layer of the enforcer: it requires
the deposit tx to spend real, mature, post-coinbase UTXOs to prove the
BTC committed for crossover was actually spendable. Coinbase outputs
fail that check.

This empirically answers the question my early research flagged as
unclear: "Can a coinbase output be a valid Thunder deposit?" The
answer is **no**, at least against the current LayerTwo-Labs enforcer.

### Architectural impact

The original PPS design's working assumption — "every block's coinbase
deposits directly to Thunder, so the pool never custodies BTC" — does
not hold. `pool_mode = pps` and its `coinbase_build_drivechain*`
builders were removed as a result; a well-formed coinbase that never
credits the Ctip only strands the block reward.

What replaced it is the **two-step deposit**: the coinbase pays the
pool's BTC P2WPKH (`pool_mode = pps-classic`), and the operator spends
accumulated coinbase UTXOs into a proper `CreateDepositTransaction`
from the admin dashboard. The pool DOES custody BTC, briefly. Full
design in [CLASSIC_PAYOUTS.md](../../CLASSIC_PAYOUTS.md).

If a future enforcer release ever permits coinbase-source deposits, the
builders are recoverable from git history — but nothing in the tree
depends on them now.

### Running it

```
scripts/regtest/setup.sh         # one-time, downloads prebuilts
scripts/regtest/start.sh
scripts/regtest/validate.sh      # activates sidechain #9, bootstraps,
                                 # prints a proxy.conf snippet
# in another terminal — start the pool with the printed config:
./build/simplepool /tmp/regtest-proxy.conf
# in a third terminal — find a block:
node scripts/regtest/cpuminer.js --timeout 60

# after a block lands, parse its coinbase:
POOL_BTC_ADDRESS=<pool_btc_address from the config> \
OPERATOR_ADDRESS=<operator_address from the config> \
    scripts/regtest/inspect-coinbase.sh

# expect:
#   [N]   value=49.5  type=witness_v0_keyhash  addr=<pool_btc_address>
#   [N+1] value=0.5   type=witness_v0_keyhash  addr=<operator_address>
#   >>> classic coinbase shape OK
```

To verify the deposit-recognition finding for yourself, side-by-side
with a canonical deposit:

```
# Ctip BEFORE a canonical deposit (after the coinbase deposit attempt):
scripts/enforcer-rpc.sh cusf.mainchain.v1.ValidatorService/GetCtip \
  '{"sidechain_number":9}'
# → {} (no Ctip — coinbase deposit was ignored)

# Issue a canonical deposit:
scripts/enforcer-rpc.sh cusf.mainchain.v1.WalletService/CreateDepositTransaction \
  '{"sidechain_id":9, "address":"11111111111111111111", "value_sats":100000000, "fee_sats":1000}'
# ... and mine it:
scripts/enforcer-rpc.sh cusf.mainchain.v1.MiningService/GenerateToAddress \
  '{"blocks":1, "address":"bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"}'

# Ctip AFTER:
scripts/enforcer-rpc.sh cusf.mainchain.v1.ValidatorService/GetCtip \
  '{"sidechain_number":9}'
# → {"ctip": {"txid": {...}, "value": "100000000"}}
```

## Why this is structured as a runbook, not a one-shot test

End-to-end BIP300 validation crosses three async processes, a sidechain
activation flow that takes multiple blocks, and a stratum miner —
wrapping all of that in a single green/red CI check would hide where
breakage actually occurred. Each script does one job loud-and-clear
so a failure points at exactly one component.

That said, there IS a one-shot composition of these scripts for CI:
`tests/test_e2e_regtest.sh` (run by
`.github/workflows/integration_tests.yaml`). It runs the minimal stack
(no thunder, walletless enforcer), prints a loud stage banner before
each step, and dumps the relevant logs on failure — so a red CI run
still points at the exact component that broke.
