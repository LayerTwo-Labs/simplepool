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

Thunder itself is intentionally NOT in this stack. There's no
aarch64-darwin prebuilt for it, and the enforcer is the authoritative
deposit validator — observing a Deposit event tagged with sidechain 9
proves the coinbase shape is correct.

## Quickstart

```
scripts/regtest/setup.sh         # download prebuilts + write configs
scripts/regtest/start.sh         # bring up bitcoind, electrs, enforcer
scripts/regtest/status.sh        # ps-style summary
scripts/regtest/validate.sh      # mine some blocks, probe GBT, print runbook
scripts/regtest/inspect-coinbase.sh   # after mining: parse tip's coinbase
scripts/regtest/stop.sh
```

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

## What's NOT yet verified end-to-end

Two prerequisites still need wiring before we can assert "a simplepool
block was accepted as a deposit":

1. **Sidechain #9 activation.** BIP300 requires a propose-then-ack
   flow before any sidechain accepts deposits. Even on regtest this
   means calling the enforcer's RPC (port 8123) to `proposesidechain`
   and then mining ~K blocks (regtest K is small) of ack votes. The
   exact RPC method names and the regtest activation window aren't
   yet captured in `validate.sh` — flagged as TODO.

2. **A real stratum miner finding work** against simplepool at regtest
   difficulty. Regtest difficulty is `0x207fffff` — trivially low — so
   any cpuminer/ckpool client wired at `127.0.0.1:13334` finds a
   block in seconds. Not scripted because miner choice depends on the
   developer's environment.

Once those two land, the final check is:

```
scripts/regtest/inspect-coinbase.sh
# expect:
#   [k]   value=49.5  OP_NOP5 OP_PUSHBYTES_1 0x09 OP_TRUE
#   [k+1] value=0     OP_RETURN <payload>
#   [k+2] value=0.5   <P2WPKH operator>
#
# then grep enforcer log:
#   "Deposit ... sidechain_id=9"
```

## Why this is structured as a runbook, not a one-shot test

End-to-end BIP300 validation crosses three async processes, a sidechain
activation flow that takes multiple blocks, and a stratum miner —
wrapping all of that in a single green/red CI check would hide where
breakage actually occurred. Each script does one job loud-and-clear
so a failure points at exactly one component.
