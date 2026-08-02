# Tests

## Unit tests

`make test` builds and runs the C unit test binaries (`tests/test_*.c`,
one `.mk` fragment each). No external processes needed. CI runs these
in `.github/workflows/check_build.yaml`.

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

Both one-shot tests allocate their stack ports dynamically per run, so
they can run concurrently — with each other and with a dev stack from
`scripts/regtest/start.sh` (which keeps the traditional fixed ports;
override via the `REGTEST_*_PORT` env vars). CI runs them as separate
jobs in `.github/workflows/integration_tests.yaml` on every PR and
push to main.
