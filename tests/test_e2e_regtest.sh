#!/usr/bin/env bash
# End-to-end test of the pps-classic mining path, one-shot for CI:
#
#   bitcoind-patched  <-ZMQ/RPC-  bip300301_enforcer (walletless)
#          ^                              | GBT 18444
#          | submitblock                  v
#          +----------------------- simplepool (pps-classic)
#                                         ^ stratum 13334
#                                         |
#                                  cpuminer.js
#
# Stages:
#   1. download + start bitcoind-patched and a walletless enforcer
#      (REGTEST_SKIP_THUNDER=1 REGTEST_WALLETLESS=1 — thunder and the
#      enforcer wallet play no part in the coinbase-shape e2e)
#   2. basic stratum smoke test against bitcoind directly
#      (tests/test_integration.sh: subscribe/authorize/reject + sqlite)
#   3. activate sidechain #9 by mining enforcer-template blocks. The pool
#      no longer emits drivechain coinbases, but keeping the sidechain
#      active means the enforcer's GBT template still carries the BIP301
#      commitment outputs the coinbase builder has to preserve.
#   4. run simplepool in pool_mode=pps-classic against the enforcer GBT
#   5. mine ONE block through the real stratum path with cpuminer.js
#   6. assert the mined tip's coinbase has the classic shape
#      (inspect-coinbase.sh: pool wallet output + operator fee output,
#      and no OP_DRIVECHAIN)
#
# Deterministic by construction: every run starts from a completely
# fresh data dir (chain state, enforcer DB, logs are wiped), while the
# downloaded binaries are cached in REGTEST_BIN_DIR across runs. The
# data dir defaults to .regtest-e2e/ so it never collides with a local
# dev stack in .regtest/ — a bare `bash tests/test_e2e_regtest.sh` is
# safe.
#
# Env:
#   REGTEST_DIR      e2e data dir, WIPED each run (default: <repo>/.regtest-e2e)
#   REGTEST_BIN_DIR  binary cache, kept across runs (default: <repo>/.regtest/bin)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
export REGTEST_DIR="${REGTEST_DIR:-$ROOT/.regtest-e2e}"
export REGTEST_BIN_DIR="${REGTEST_BIN_DIR:-$ROOT/.regtest/bin}"
export REGTEST_SKIP_THUNDER=1
export REGTEST_WALLETLESS=1

BIN="$REGTEST_BIN_DIR"
POOL_BIN="$ROOT/build/simplepool"
POOL_CONF="/tmp/simplepool-e2e.conf"
POOL_LOG="/tmp/simplepool-e2e.log"
POOL_DB="/tmp/simplepool-e2e.db"
POOL_PORT=13334
OPERATOR_ADDR="bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"
# The pool's own BTC wallet — where pps-classic sends the net-of-fee
# reward. Must differ from OPERATOR_ADDR so the assertion below can tell
# the two coinbase outputs apart.
POOL_BTC_ADDR="bcrt1qqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5phstwt"
POOL_PID=""

cli() { "$BIN/bitcoin-cli" -datadir="$REGTEST_DIR/data/bitcoind" -regtest \
        -rpcuser=user -rpcpassword=password "$@"; }

stage() { echo; echo "=== e2e: $1"; }

dump_logs() {
    echo "!!! e2e FAILED — recent logs:" >&2
    for f in "$REGTEST_DIR"/logs/*.log "$POOL_LOG"; do
        [ -f "$f" ] || continue
        echo "--- tail $f" >&2
        tail -40 "$f" >&2
    done
}

cleanup() {
    [ -n "$POOL_PID" ] && kill "$POOL_PID" 2>/dev/null || true
    "$ROOT/scripts/regtest/stop.sh" || true
}
trap 'code=$?; [ "$code" -ne 0 ] && dump_logs; cleanup; exit $code' EXIT

stage "check stack ports are free"
# The stack uses fixed ports, so REGTEST_DIR isolation is not enough: if
# another regtest stack is already running, the wait_for probes in
# start.sh would silently cross-wire this test into it (and mine blocks
# on its chain). Refuse to run instead.
for p in 18443 18444 50051 "$POOL_PORT"; do
    if nc -z 127.0.0.1 "$p" 2>/dev/null; then
        echo "FAIL: port $p is already in use — is another regtest stack" >&2
        echo "running? (scripts/regtest/stop.sh)" >&2
        trap - EXIT
        exit 1
    fi
done

stage "wipe e2e data dir (fresh chain every run)"
# Chain state must not leak between runs: a pre-activated sidechain or
# an aged chain changes what the later stages actually exercise (e.g.
# median-time-past vs curtime). Binaries in $BIN are the only cache.
rm -rf "$REGTEST_DIR/data" "$REGTEST_DIR/logs" "$REGTEST_DIR/run"

stage "build simplepool"
make -C "$ROOT" -j >/dev/null

stage "download prebuilt binaries"
"$ROOT/scripts/regtest/setup.sh"

stage "start bitcoind-patched + walletless enforcer"
"$ROOT/scripts/regtest/start.sh"

stage "basic stratum smoke test (solo mode, direct bitcoind)"
PATH="$BIN:$PATH" BITCOIND_USER=user BITCOIND_PASS=password \
    bash "$HERE/test_integration.sh"

stage "activate sidechain #9 via enforcer-template mining"
"$ROOT/scripts/regtest/activate-thunder.sh"

stage "start simplepool in pps-classic mode against enforcer GBT"
rm -f "$POOL_DB"
cat > "$POOL_CONF" <<EOF
listen_addr = 127.0.0.1
listen_port = ${POOL_PORT}

bitcoind_url = http://127.0.0.1:18444
bitcoind_poll_interval_ms = 500

operator_address = ${OPERATOR_ADDR}
fee_bps = 100
coinbase_tag = /simplepool-e2e/

pool_mode = pps-classic
pool_btc_address = ${POOL_BTC_ADDR}
# The share difficulty is clamped to the network difficulty, which on
# regtest is ~4.66e-10 — so the rate must be huge for a share to accrue
# whole sats (credit = trunc(difficulty * pps_sats_per_diff), 0 is
# dropped). 1e10 * 4.66e-10 ≈ 4 sats.
pps_sats_per_diff = 10000000000

# Clamped down to the network difficulty at connect time, so any nonce
# that finds a block also passes the share check (see cpuminer.js)
initial_diff = 0.0000001
vardiff_enabled = 0

db_path = ${POOL_DB}
log_level = debug
EOF
"$POOL_BIN" "$POOL_CONF" > "$POOL_LOG" 2>&1 &
POOL_PID=$!
for _ in $(seq 1 20); do nc -z 127.0.0.1 "$POOL_PORT" 2>/dev/null && break; sleep 1; done
if ! kill -0 "$POOL_PID" 2>/dev/null; then
    echo "simplepool died on startup" >&2
    exit 1
fi

stage "mine one block through stratum with cpuminer.js"
TIP_BEFORE=$(cli getblockcount)
node "$ROOT/scripts/regtest/cpuminer.js" --port "$POOL_PORT" --timeout 120
TIP_AFTER=$(cli getblockcount)
echo "height: $TIP_BEFORE -> $TIP_AFTER"
if [ "$TIP_AFTER" -le "$TIP_BEFORE" ]; then
    echo "FAIL: block was submitted but the chain did not advance" >&2
    exit 1
fi

stage "assert pps-classic coinbase shape on the new tip"
POOL_BTC_ADDRESS="$POOL_BTC_ADDR" OPERATOR_ADDRESS="$OPERATOR_ADDR" \
    "$ROOT/scripts/regtest/inspect-coinbase.sh"

stage "assert pool DB recorded the accepted share"
# give the batched writer a moment, then stop the pool cleanly to flush
sleep 1
kill -INT "$POOL_PID" 2>/dev/null || true
for _ in 1 2 3 4 5; do kill -0 "$POOL_PID" 2>/dev/null || break; sleep 1; done
POOL_PID=""
WORKERS=$(sqlite3 "$POOL_DB" "SELECT count(*) FROM workers WHERE payout_address IS NOT NULL")
SHARES=$(sqlite3 "$POOL_DB" "SELECT count(*) FROM shares")
BLOCKS=$(sqlite3 "$POOL_DB" "SELECT count(*) FROM blocks_found")
CREDITS=$(sqlite3 "$POOL_DB" "SELECT count(*) FROM pps_credits WHERE accrued_sats > 0")
echo "workers=$WORKERS shares=$SHARES blocks_found=$BLOCKS pps_credits=$CREDITS"
[ "$WORKERS" -ge 1 ] || { echo "FAIL: no worker with payout_address" >&2; exit 1; }
[ "$SHARES"  -ge 1 ] || { echo "FAIL: no accepted shares" >&2; exit 1; }
[ "$BLOCKS"  -ge 1 ] || { echo "FAIL: no blocks_found row" >&2; exit 1; }
[ "$CREDITS" -ge 1 ] || { echo "FAIL: no pps credit accrued" >&2; exit 1; }

stage "PASS"
