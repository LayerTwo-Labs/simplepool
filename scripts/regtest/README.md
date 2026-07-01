# BIP300 regtest validation stack

Local stack for validating that simplepool's `pool_mode=pps` coinbase
shape is accepted as a valid drivechain deposit by the canonical
LayerTwo-Labs enforcer.

## Stack

```
                       ┌──────────────┐  GBT 18444
                       │  enforcer    │◀────── simplepool (pps mode)
                       │              │
              ┌────────┤              ├────────┐
              │  ZMQ   │              │ gRPC   │
              │ 29000  └──────────────┘ 50051  │
              ▼              │                 ▼
        ┌──────────┐         │           (events,
        │ bitcoind │◀────────┴───────┐   sidechain CRUD)
        │ regtest  │  RPC 18443      │
        │ patched  │                 │
        └──────────┘                 │
              ▲                      │
              │ wallet sync          │
              │                      │
              └──────────┬───────────┘
                         │ electrum 60401
                   ┌─────┴────┐
                   │ electrs  │
                   └──────────┘
```

- **bitcoind-patched** (v30.2): BIP300/301-aware Bitcoin Core fork, regtest mode.
- **electrs**: Electrum server the enforcer's wallet uses for sync.
- **bip300301_enforcer**: validates BIP300 deposits, serves the
  `getblocktemplate` simplepool talks to.
- **thunder** (L2-S9): the actual sidechain node, connected to the
  enforcer via gRPC on 50051, RPC on 6009 (matches what
  `payout/lib/thunder.js` expects out of the box).

## Quickstart

```
scripts/regtest/setup.sh             # download prebuilts + write configs
scripts/regtest/start.sh             # bring up bitcoind, electrs, enforcer, thunder
scripts/regtest/status.sh            # ps-style summary
scripts/regtest/activate-thunder.sh  # propose + ack sidechain #9 until active
scripts/regtest/thunder-init.sh      # generate Thunder wallet mnemonic + address
scripts/regtest/validate.sh          # activate, mine 150, probe GBT, print runbook
scripts/regtest/inspect-coinbase.sh  # after mining: parse tip's coinbase
scripts/regtest/stop.sh
```

`activate-thunder.sh` and `thunder-init.sh` are both idempotent —
re-running once the state is set up is a no-op.
Requires `grpcurl` (`brew install grpcurl`).

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
([`cpuminer.js`](cpuminer.js)) connects to a `pool_mode=pps` simplepool
running against the enforcer, finds a regtest block in seconds, and
submits it. The block is accepted into the regtest chain by bitcoind-
patched and the enforcer; bitcoind classifies the coinbase output as
`"type": "drivechain"`; the OP_RETURN destination immediately follows;
`inspect-coinbase.sh` confirms the 5-output layout.

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

The PPS design's working assumption — "every block's coinbase deposits
directly to Thunder, so the pool never custodies BTC" — needs revision.
Options for the follow-up:

1. **Two-step deposit.** Coinbase pays the pool's BTC P2WPKH; a separate
   service spends accumulated coinbase UTXOs into a proper
   `CreateDepositTransaction` periodically. Pool DOES custody BTC,
   briefly. Lowest implementation cost.
2. **Per-block deposit tx.** Pool builds and broadcasts a deposit tx
   in the same block as the coinbase. Higher coordination cost.
3. **Re-examine the enforcer's deposit rule for a path that does work**
   (e.g. is there a flag, or did the rule change in a recent release?).

The drivechain coinbase builders we landed are still useful — they
produce a well-formed (if not Ctip-crediting) coinbase shape, and the
parsing/structural assertions stand. The shape becomes useful again
if option 3 turns up a way to make the rule permit it.

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
scripts/regtest/inspect-coinbase.sh

# expect:
#   output count: 5
#   [N]   value=49.5  type=drivechain  asm=OP_NOP5 9 1
#   [N+1] value=0     type=nulldata    asm=OP_RETURN <payload>
#   [N+2] value=0.5   type=witness_v0_keyhash
```

To verify the deposit-recognition finding for yourself, side-by-side
with a canonical deposit:

```
# Ctip BEFORE a canonical deposit (after the coinbase deposit attempt):
grpcurl -plaintext -d '{"sidechain_number":9}' 127.0.0.1:50051 \
  cusf.mainchain.v1.ValidatorService/GetCtip
# → {} (no Ctip — coinbase deposit was ignored)

# Issue a canonical deposit:
grpcurl -plaintext -d '{"sidechain_id":9, "address":"11111111111111111111", "value_sats":100000000, "fee_sats":1000}' \
  127.0.0.1:50051 cusf.mainchain.v1.WalletService/CreateDepositTransaction
grpcurl -plaintext -d '{"blocks":1}' 127.0.0.1:50051 \
  cusf.mainchain.v1.WalletService/GenerateBlocks

# Ctip AFTER:
grpcurl -plaintext -d '{"sidechain_number":9}' 127.0.0.1:50051 \
  cusf.mainchain.v1.ValidatorService/GetCtip
# → {"ctip": {"txid": {...}, "value": "100000000"}}
```

## Why this is structured as a runbook, not a one-shot test

End-to-end BIP300 validation crosses three async processes, a sidechain
activation flow that takes multiple blocks, and a stratum miner —
wrapping all of that in a single green/red CI check would hide where
breakage actually occurred. Each script does one job loud-and-clear
so a failure points at exactly one component.
