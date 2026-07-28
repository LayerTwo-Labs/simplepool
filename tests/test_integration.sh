#!/usr/bin/env bash
# Best-effort integration test for simplepool against a local regtest bitcoind.
# Skips (exit 0) if bitcoin-cli or a running regtest node is unavailable.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BIN="$ROOT/build/simplepool"
DB="/tmp/simplepool-int.db"
CONF="$HERE/integration.proxy.conf"
PORT="13334"
RPC_HOST="127.0.0.1"
RPC_PORT="18443"
RPC_USER="${BITCOIND_USER:-drivepool}"
RPC_PASS="${BITCOIND_PASS:-drivepool}"
# `ADDR` is the regtest payout we mine pre-101 coinbases to (warm-up only)
# AND the operator/fee recipient in the proxy config. `MINER_ADDR` is the
# stratum username — i.e. the per-miner payout address.
ADDR="${SIMPLEPOOL_OPERATOR_ADDR:-bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080}"
MINER_ADDR="${SIMPLEPOOL_MINER_ADDR:-bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080}"

skip() { echo "SKIP: $1"; exit 0; }

command -v bitcoin-cli >/dev/null 2>&1 || skip "bitcoin-cli not installed"

CLI="bitcoin-cli -regtest -rpcconnect=${RPC_HOST} -rpcport=${RPC_PORT} -rpcuser=${RPC_USER} -rpcpassword=${RPC_PASS}"

if ! $CLI getblockchaininfo >/dev/null 2>&1; then
    skip "no regtest bitcoind on ${RPC_HOST}:${RPC_PORT}"
fi

# Mine 101 blocks if needed so coinbase is spendable / templates are non-empty.
HEIGHT=$($CLI getblockcount)
if [ "$HEIGHT" -lt 101 ]; then
    NEED=$((101 - HEIGHT))
    echo "Mining $NEED blocks to $ADDR..."
    $CLI generatetoaddress "$NEED" "$ADDR" >/dev/null
fi

# Make sure binary is built.
if [ ! -x "$BIN" ]; then
    echo "building $BIN..."
    (cd "$ROOT" && make >/dev/null)
fi

# Fresh DB.
rm -f "$DB"

# Write the config.
cat > "$CONF" <<EOF
listen_addr = 127.0.0.1
listen_port = ${PORT}

bitcoind_url = http://${RPC_HOST}:${RPC_PORT}
bitcoind_user = ${RPC_USER}
bitcoind_pass = ${RPC_PASS}
# keep this short: the tip-watcher thread sleeps in poll-interval chunks,
# so shutdown can take up to one full interval to join it
bitcoind_poll_interval_ms = 1000

operator_address = ${ADDR}
fee_bps = 100
coinbase_tag = /simplepool-int/

db_path = ${DB}
commit_window_ms = 50
commit_max_shares = 50

log_level = info
EOF

# Start the proxy.
"$BIN" "$CONF" >/tmp/simplepool-int.log 2>&1 &
PID=$!
trap '[ -n "${PID:-}" ] && kill "$PID" 2>/dev/null || true' EXIT
sleep 2

if ! kill -0 "$PID" 2>/dev/null; then
    echo "FAIL: simplepool died on startup; log:"
    cat /tmp/simplepool-int.log
    exit 1
fi

# Tiny stratum client — just exercise subscribe/authorize + a stale submit.
NC=$(command -v nc || true)
if [ -z "$NC" ]; then
    skip "nc not available"
fi

{
    printf '{"id":1,"method":"mining.subscribe","params":["simplepool-int/1.0"]}\n'
    sleep 0.2
    printf '{"id":2,"method":"mining.authorize","params":["%s.rig1","x"]}\n' "$MINER_ADDR"
    sleep 0.2
    printf '{"id":3,"method":"mining.submit","params":["%s.rig1","badjob","00000000","deadbeef","11223344"]}\n' "$MINER_ADDR"
    sleep 0.5
} | $NC -w 2 127.0.0.1 "$PORT" >/tmp/simplepool-int.client.log 2>&1 || true

# Give the writer thread a moment to flush.
sleep 1
kill -INT "$PID" 2>/dev/null || true

# Wait for orderly shutdown (up to 5s), then force-kill so the stratum
# port is guaranteed free for whatever runs after us.
for _ in 1 2 3 4 5; do
    if ! kill -0 "$PID" 2>/dev/null; then break; fi
    sleep 1
done
kill -9 "$PID" 2>/dev/null || true
trap - EXIT

if ! command -v sqlite3 >/dev/null 2>&1; then
    skip "sqlite3 cli not installed"
fi

# Workers rows are only created when a share is ACCEPTED (resolve_worker_id
# runs on share/block/pps events, not on authorize), and our nc client can't
# mine a real share — so the only DB row this flow produces is the reject
# for the bogus submit. Worker/share row assertions live in
# tests/test_e2e_regtest.sh, where cpuminer.js produces an accepted share.
REJECTS=$(sqlite3 "$DB" "SELECT count(*) FROM rejects" 2>/dev/null || echo 0)

echo "rejects=$REJECTS"
if [ "$REJECTS" -lt 1 ]; then
    echo "FAIL: expected >= 1 reject"; cat /tmp/simplepool-int.log; exit 1
fi

echo "PASS"
exit 0
