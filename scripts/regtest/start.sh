#!/usr/bin/env bash
# Start the regtest stack: bitcoind-patched, electrs, bip300301_enforcer.
#
# Each process gets a pidfile under .regtest/run/ and a logfile under
# .regtest/logs/. Re-running this script is a no-op for processes whose
# pidfile is alive (idempotent).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REGTEST="$ROOT/.regtest"
BIN="$REGTEST/bin"
DATA="$REGTEST/data"
LOGS="$REGTEST/logs"
RUN="$REGTEST/run"
mkdir -p "$RUN"

for b in bitcoind bitcoin-cli bip300301_enforcer electrs thunder thunder-cli; do
    if [[ ! -x "$BIN/$b" ]]; then
        echo "missing $BIN/$b — run scripts/regtest/setup.sh first" >&2
        exit 1
    fi
done

is_alive() {
    local pid="$1"
    [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null
}

start_if_dead() {
    local name="$1"; shift
    local pidfile="$RUN/$name.pid"
    if [[ -f "$pidfile" ]] && is_alive "$(cat "$pidfile")"; then
        echo "  $name already running (pid $(cat "$pidfile"))"
        return
    fi
    echo "  starting $name"
    "$@" >> "$LOGS/$name.log" 2>&1 &
    echo $! > "$pidfile"
    disown $!
}

wait_for() {
    local name="$1"; local check="$2"; local timeout="${3:-30}"
    for ((i = 0; i < timeout; i++)); do
        if eval "$check" >/dev/null 2>&1; then
            echo "  $name ready"
            return
        fi
        sleep 1
    done
    echo "  $name failed to come up in ${timeout}s; check $LOGS/$name.log" >&2
    exit 2
}

echo "==> starting bitcoind"
start_if_dead bitcoind \
    "$BIN/bitcoind" \
    -datadir="$DATA/bitcoind" \
    -conf="$DATA/bitcoind/bitcoin.conf" \
    -daemonwait=0

wait_for bitcoind "$BIN/bitcoin-cli -datadir=$DATA/bitcoind -regtest \
    -rpcuser=user -rpcpassword=password getblockchaininfo"

# Create / load the miner wallet (descriptor wallet is the only supported
# kind on Bitcoin Core v30). Idempotent — already-loaded is fine.
"$BIN/bitcoin-cli" -datadir="$DATA/bitcoind" -regtest \
    -rpcuser=user -rpcpassword=password \
    -named createwallet wallet_name=miner descriptors=true 2>/dev/null \
    || "$BIN/bitcoin-cli" -datadir="$DATA/bitcoind" -regtest \
       -rpcuser=user -rpcpassword=password loadwallet miner true 2>/dev/null \
    || true

echo "==> starting electrs"
start_if_dead electrs \
    "$BIN/electrs" \
    --network regtest \
    --daemon-dir "$DATA/bitcoind" \
    --daemon-rpc-addr 127.0.0.1:18443 \
    --cookie "user:password" \
    --db-dir "$DATA/electrs/db" \
    --electrum-rpc-addr 127.0.0.1:60401 \
    --jsonrpc-import \
    --timestamp

# electrs takes a moment to bind its rpc port; probe via netcat-ish curl.
wait_for electrs "nc -z 127.0.0.1 60401" 20

echo "==> starting bip300301_enforcer"
start_if_dead bip300301_enforcer \
    "$BIN/bip300301_enforcer" \
    --data-dir="$DATA/enforcer" \
    --enable-wallet \
    --enable-mempool \
    --wallet-auto-create \
    --wallet-sync-source=electrum \
    --wallet-electrum-host=127.0.0.1 \
    --wallet-electrum-port=60401 \
    --node-rpc-addr=127.0.0.1:18443 \
    --node-rpc-user=user \
    --node-rpc-pass=password \
    --node-zmq-addr-sequence=tcp://127.0.0.1:29000 \
    --serve-rpc-addr=127.0.0.1:18444 \
    --serve-json-rpc-addr=127.0.0.1:8123 \
    --serve-grpc-addr=127.0.0.1:50051

wait_for bip300301_enforcer "nc -z 127.0.0.1 18444" 30
# Thunder connects to the enforcer's gRPC — make sure that's actually
# up before launching it (18444 GBT can come up first).
wait_for enforcer-grpc "nc -z 127.0.0.1 50051" 15

echo "==> starting thunder (sidechain #9)"
start_if_dead thunder \
    "$BIN/thunder" \
    --headless \
    --datadir "$DATA/thunder" \
    --network regtest \
    --mainchain-grpc-url http://127.0.0.1:50051 \
    --net-addr 127.0.0.1:4009 \
    --rpc-addr 127.0.0.1:6009 \
    --log-level INFO

wait_for thunder "nc -z 127.0.0.1 6009" 30

echo ""
echo "stack up. endpoints:"
echo "  bitcoind RPC:    127.0.0.1:18443  (user/password)"
echo "  electrs:         127.0.0.1:60401"
echo "  enforcer GBT:    127.0.0.1:18444  (point simplepool at this)"
echo "  enforcer JSONRPC: 127.0.0.1:8123"
echo "  enforcer gRPC:    127.0.0.1:50051"
echo "  thunder RPC:     127.0.0.1:6009   (point payout worker at this)"
echo "  thunder P2P:     127.0.0.1:4009"
echo ""
echo "next: scripts/regtest/validate.sh"
