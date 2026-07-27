# Tests

## Unit tests

`make test` builds and runs the C unit test binaries (`tests/test_*.c`,
one `.mk` fragment each). No external processes needed. CI runs these
in `.github/workflows/check_build.yaml`.

## Integration tests

- `test_integration.sh` — smoke test of the stratum surface against a
  running regtest bitcoind (subscribe/authorize/bogus-submit via nc,
  then asserts the reject landed in SQLite). Best-effort: skips cleanly
  when bitcoind/nc/sqlite3 are unavailable.
- `test_e2e_regtest.sh` — the full drivechain mining path, one-shot:
  downloads bitcoind-patched + bip300301_enforcer (walletless), starts
  the minimal regtest stack, runs the smoke test above, activates
  sidechain #9 (`SetAckAllProposals` + the walletless
  `MiningService/GenerateToAddress`, enforcer PR #477), then runs
  simplepool in `pool_mode=pps` and mines a real block through stratum
  with `scripts/regtest/cpuminer.js`. Asserts the drivechain coinbase
  shape and the pool DB rows (worker, share, block, pps credit).

  Deterministic by construction: chain state lives in its own
  `.regtest-e2e/` dir and is wiped at the start of every run, while
  downloaded binaries are cached (default: `.regtest/bin`, shared with
  the dev stack; override with `REGTEST_BIN_DIR`). So this is safe and
  repeatable as-is — it never touches a dev chain in `.regtest/`:

      bash tests/test_e2e_regtest.sh

CI runs the e2e in `.github/workflows/integration_tests.yaml` on every
PR and push to main.
