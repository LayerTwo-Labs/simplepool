# Tests

## Unit tests

`make test` builds and runs the C unit test binaries (`tests/test_*.c`,
one `.mk` fragment each). No external processes needed. CI runs these
in `.github/workflows/check_build.yaml`.

Two of them exist because the logic they cover was once unreachable from any
test, sitting in a `static` function inside `main.c`:

- `test_reconcile.c` — the block-confirmation pass (`src/reconcile.c`).
- `test_pplns.c` — the window-to-payees split (`src/pplns.c`): the fee, the
  proportional division, the rounding remainder and the payout-floor
  prediction. It is also the only place the **mixed-window** cases can be
  stated exactly, because on regtest the window holds about two shares (share
  difficulty is clamped to network difficulty), so a chain cannot produce a
  window of wildly different claim sizes without help.

`make asan` runs a subset under AddressSanitizer + UBSan; `make coverage`
reports line and function coverage of the unit suites only.

## Integration tests

Upstream binary versions are pinned in the pinned-versions block of
`scripts/regtest/setup.sh` (thunder by GitHub release tag, the L1 node
by versioned zip). Bump them there — deliberately, with a revalidating
run — when moving to a new upstream. The enforcer is the exception
(latest-only artifacts upstream); setup.sh warns when it drifts from
the version last validated against.

- `test_integration.sh` — smoke test of the stratum surface against a
  running regtest bitcoind (subscribe/authorize/bogus-submit via nc,
  then asserts the reject landed in SQLite). Best-effort: skips cleanly
  when bitcoind/nc/sqlite3 are unavailable.
- `test_e2e_regtest.sh` — the full mining path, one-shot: downloads
  bitcoind-patched + bip300301_enforcer (walletless), starts the minimal
  regtest stack, runs the smoke test above, activates sidechain #9
  (`SetAckAllProposals` + the walletless
  `MiningService/GenerateToAddress`, enforcer PR #477) so the GBT
  template carries BIP301 commitments, then runs simplepool in
  `pool_mode=pps-classic` and mines a real block through stratum with
  `scripts/regtest/cpuminer.js`. Asserts the classic coinbase shape
  (pool wallet output + operator fee, no OP_DRIVECHAIN) and the pool DB
  rows (worker, share, block, pps credit).

  Deterministic by construction: chain state lives in its own
  `.regtest-e2e/` dir and is wiped at the start of every run, while
  downloaded binaries are cached (default: `.regtest/bin`, shared with
  the dev stack; override with `REGTEST_BIN_DIR`). So this is safe and
  repeatable as-is — it never touches a dev chain in `.regtest/`:

      bash tests/test_e2e_regtest.sh

- `test_payout_regtest.sh` — the Thunder payout path the coinbase e2e
  skips: wallet-ENABLED enforcer + thunder, sidechain #9 activated, a
  real `CreateDepositTransaction` moving 1 BTC into thunder (BMM-mining
  a thunder block to credit it), then one payout tick via
  `payout/run-once.mjs`. Asserts the at-most-once ledger settled
  (payouts row with txid, `paid_sats` bumped, no in-flight rows),
  thunder's `get_transaction` knows the txid, and the reserve balance
  math is exact. Needs node ≥ 20. Same isolation model, own
  `.regtest-payout/` dir:

      bash tests/test_payout_regtest.sh

- `test_solo_regtest.sh` — `pool_mode=solo`, the default and the mode most
  operators run. Two miners with two **different** addresses mine a block
  each, and each block's coinbase must pay its own finder: that is the
  defining property of solo, and a regression rendering one coinbase for
  every connection would still pass a single-miner test. Also asserts the
  enforcer's commitments survived, that nothing was credited off-chain, and
  that the pool reported `mode=solo` — which pins the default, since the
  config sets no `pool_mode` at all. Own `.regtest-solo/` dir.

- `test_pplns_regtest.sh` — both custodial PPLNS rails, and both
  confirmation paths. Mines to maturity and asserts a matured block is
  distributed across its window exactly once. Own `.regtest-pplns/` dir.

- `test_pplns_btc_payout_regtest.sh` — the L1 payout rail: three miners, one
  batched transaction through the enforcer's wallet, with a shared address
  summed. Own `.regtest-btcpay/` dir.

- `test_pplns_coinbase_regtest.sh` — `pool_mode=pplns-coinbase`, which has no
  ledger step at all: the payment IS the block. Asserts the coinbase pays the
  window on chain, that no output pays anything the pool controls beyond its
  fee, that `pps_credits` stays empty, and that the payout floor is disclosed
  at startup, per template and per block.

  Its last stage is the one worth knowing about. A **mixed** window — claims
  of 100 : 10 : 1 with the floor between the last two — cannot be mined for
  on regtest, because a 2.0x window holds about two shares. So that stage
  widens the window multiple and seeds the shares table directly, with the
  pool stopped. That is replaying the pool's own record of accepted work, not
  stubbing what is under test: the window query, the split, the builder, the
  block and the outputs read back off the chain are all real, and the
  redistributed amount is asserted against the arithmetic — as is the operator
  holding exactly its fee, and the payout queue summing to zero. Own
  `.regtest-cbwin/` dir.

Every one-shot test allocates its stack ports dynamically per run, so
they can run concurrently — with each other and with a dev stack from
`scripts/regtest/start.sh` (which keeps the traditional fixed ports;
override via the `REGTEST_*_PORT` env vars). CI runs them as separate
jobs in `.github/workflows/integration_tests.yaml` on every PR and
push to main.

One caveat about `test_integration.sh`, first in the list above: it looks
like a solo end-to-end test and is not. It never mines, so it cannot see
whether a coinbase pays the right person, and it is not in CI. That is what
`test_solo_regtest.sh` was written for.

## Generated documentation

The sequence diagrams in `docs/simplepool.html` are inline SVG produced by
`docs/sequence-diagrams.py` — no library, no external requests, colours from
the page's own CSS variables so they follow the reader's theme. Do not edit
the SVG by hand: it is ~4 KB per diagram of computed coordinates, and the
script owns the layout, the note wrapping and the element ids.

    python3 docs/sequence-diagrams.py            # rewrite the diagrams in place
    python3 docs/sequence-diagrams.py --check    # fail if they are out of date

To change a diagram, edit the `DIAGRAMS` spec at the bottom of that script and
re-run it. Output is deterministic, so a no-op run leaves the file
byte-identical — which is what `--check` relies on, and what CI runs in
`check_build.yaml`.

That guard exists because the diagrams have gone stale once already: the
`pplns-coinbase` rule that a claim too small to pay is forfeited to the
operator was reversed, and "FORFEITED to the operator" stayed drawn into the
picture. A diagram nobody can regenerate is a diagram that quietly stops being
true.
