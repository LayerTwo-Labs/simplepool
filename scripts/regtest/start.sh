#!/usr/bin/env bash
# Start the regtest stack: bitcoind-patched, bip300301_enforcer, thunder.
#
# Each process gets a pidfile under .regtest/run/ and a logfile under
# .regtest/logs/. Re-running this script is a no-op for processes whose
# pidfile is alive (idempotent).
#
# Env:
#   REGTEST_DIR           chain state/log location (default: <repo>/.regtest)
#   REGTEST_BIN_DIR       binary cache (default: $REGTEST_DIR/bin)
#   REGTEST_SKIP_THUNDER  =1 to not start thunder (CI)
#   REGTEST_WALLETLESS    =1 to run the enforcer without its wallet.
#                         Template rewards then go to a fixed regtest
#                         address and WalletService RPCs (e.g.
#                         CreateDepositTransaction) are unavailable.
#                         Mine with MiningService/GenerateToAddress
#                         (enforcer PR #477).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REGTEST="${REGTEST_DIR:-$ROOT/.regtest}"
BIN="${REGTEST_BIN_DIR:-$REGTEST/bin}"
DATA="$REGTEST/data"
LOGS="$REGTEST/logs"
RUN="$REGTEST/run"
SKIP_THUNDER="${REGTEST_SKIP_THUNDER:-0}"
WALLETLESS="${REGTEST_WALLETLESS:-0}"
mkdir -p "$RUN"

# Stack ports. Env-overridable so several stacks can coexist — the
# integration tests allocate free ports per run; these defaults are the
# traditional dev-stack ports. Must match what setup.sh wrote into
# bitcoin.conf, so export the same REGTEST_* vars to both scripts.
BITCOIND_RPC_PORT="${REGTEST_BITCOIND_RPC_PORT:-18443}"
BITCOIND_ZMQ_PORT="${REGTEST_BITCOIND_ZMQ_PORT:-29010}"
ENFORCER_RPC_PORT="${REGTEST_ENFORCER_RPC_PORT:-18444}"
ENFORCER_GRPC_PORT="${REGTEST_ENFORCER_GRPC_PORT:-50051}"
THUNDER_RPC_PORT="${REGTEST_THUNDER_RPC_PORT:-6009}"
THUNDER_P2P_PORT="${REGTEST_THUNDER_P2P_PORT:-4009}"

BINARIES=(bitcoind bitcoin-cli bip300301_enforcer)
[[ "$SKIP_THUNDER" == 1 ]] || BINARIES+=(thunder thunder-cli)
for b in "${BINARIES[@]}"; do
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

echo "==> starting bip300301_enforcer"
# No electrs / electrum sync: with --wallet-sync-source=disabled the wallet
# is updated purely by incoming blocks, which is complete on a from-genesis
# regtest chain. Walletless mode drops the wallet entirely; the template
# server then needs an explicit --coinbase-recipient (any valid regtest
# address — the pool builds its own coinbase anyway).
if [[ "$WALLETLESS" == 1 ]]; then
    WALLET_ARGS=(--coinbase-recipient=bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080)
else
    WALLET_ARGS=(--enable-wallet --wallet-auto-create --wallet-sync-source=disabled)
fi
start_if_dead bip300301_enforcer \
    "$BIN/bip300301_enforcer" \
    --data-dir="$DATA/enforcer" \
    --enable-mempool \
    "${WALLET_ARGS[@]}" \
    --node-rpc-addr=127.0.0.1:$BITCOIND_RPC_PORT \
    --node-rpc-user=user \
    --node-rpc-pass=password \
    --node-zmq-addr-sequence=tcp://127.0.0.1:$BITCOIND_ZMQ_PORT \
    --enable-block-template-server \
    --serve-rpc-addr=127.0.0.1:$ENFORCER_RPC_PORT \
    --serve-grpc-addr=127.0.0.1:$ENFORCER_GRPC_PORT

wait_for bip300301_enforcer "nc -z 127.0.0.1 $ENFORCER_RPC_PORT" 30
# Thunder connects to the enforcer's gRPC — make sure that's actually
# up before launching it (the GBT port can come up first).
wait_for enforcer-grpc "nc -z 127.0.0.1 $ENFORCER_GRPC_PORT" 15

if [[ "$SKIP_THUNDER" != 1 ]]; then
    echo "==> starting thunder (sidechain #9)"
    start_if_dead thunder \
        "$BIN/thunder" \
        --headless \
        --datadir "$DATA/thunder" \
        --network regtest \
        --mainchain-grpc-url http://127.0.0.1:$ENFORCER_GRPC_PORT \
        --net-addr 127.0.0.1:$THUNDER_P2P_PORT \
        --rpc-addr 127.0.0.1:$THUNDER_RPC_PORT \
        --log-level INFO

    wait_for thunder "nc -z 127.0.0.1 $THUNDER_RPC_PORT" 30
fi

echo ""
echo "stack up. endpoints:"
echo "  bitcoind RPC:    127.0.0.1:$BITCOIND_RPC_PORT  (user/password)"
echo "  enforcer GBT:    127.0.0.1:$ENFORCER_RPC_PORT  (point simplepool at this)"
echo "  enforcer gRPC:    127.0.0.1:$ENFORCER_GRPC_PORT"
if [[ "$SKIP_THUNDER" != 1 ]]; then
    echo "  thunder RPC:     127.0.0.1:$THUNDER_RPC_PORT   (point payout worker at this)"
    echo "  thunder P2P:     127.0.0.1:$THUNDER_P2P_PORT"
fi
echo ""
echo "next: scripts/regtest/validate.sh"
