#!/usr/bin/env bash
# End-to-end proof of the slipstream service, against a real chain.
#
#   bitcoind-patched  <->  bip300301_enforcer --enable-slipstream  <->  slipstream/
#
# What only a chain can prove, and what this asserts:
#
#   1. a tx the node refuses to relay (non-standard: a dust output) is
#      accepted through slipstream, and never enters the node's mempool.
#      Nothing is relayed, which is the point of the service.
#   2. it reaches the template the enforcer serves -- the one the proxy
#      mines -- and the service sees it there (in_template).
#   3. mining that template confirms it, and the service follows it through
#      mined to confirmed, with bitcoind supplying the depth.
#   4. the fee rule holds: a tx under the floor is refused, and taken back
#      out of the enforcer so it cannot be mined for less than was asked.
#   5. every submission is kept, refused ones included.
#   6. info.json names the slipstream URL and the software.
#
# The proxy is not run: the service talks only to the enforcer and bitcoind,
# and the template is mined directly with generateblock, which accepts a
# block only if the node finds the whole of it valid.
#
# Env:
#   SLIPSTREAM_ENFORCER_BIN  REQUIRED until an enforcer release ships
#                            --enable-slipstream (LayerTwo-Labs/
#                            bip300301_enforcer#642): a bip300301_enforcer
#                            binary built from that branch.
#   REGTEST_DIR      data dir, WIPED each run (default: <repo>/.regtest/slipstream-e2e)
#   REGTEST_BIN_DIR  binary cache for bitcoind (default: <repo>/.regtest/bin)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
export REGTEST_DIR="${REGTEST_DIR:-$ROOT/.regtest/slipstream-e2e}"
CACHE_BIN_DIR="${REGTEST_BIN_DIR:-$ROOT/.regtest/bin}"
export REGTEST_SKIP_THUNDER=1
export REGTEST_WALLETLESS=1
export REGTEST_ENFORCER_EXTRA_ARGS="--enable-slipstream"

: "${SLIPSTREAM_ENFORCER_BIN:?set SLIPSTREAM_ENFORCER_BIN to an enforcer built with --enable-slipstream}"
[ -x "$SLIPSTREAM_ENFORCER_BIN" ] || { echo "not executable: $SLIPSTREAM_ENFORCER_BIN" >&2; exit 1; }

SVC_LOG="$REGTEST_DIR/slipstream.log"
SVC_DB="$REGTEST_DIR/slipstream.db"
SVC_PID=""
BIN="$REGTEST_DIR/bin"

cli()   { "$BIN/bitcoin-cli" -datadir="$REGTEST_DIR/data/bitcoind" -regtest \
          -rpcuser=user -rpcpassword=password "$@"; }
wcli()  { cli -rpcwallet=miner "$@"; }
stage() { echo; echo "=== slipstream-e2e: $1"; }
fail()  { echo "FAIL: $*" >&2; exit 1; }
api()   { curl -sf "http://127.0.0.1:$SVC_PORT$1"; }
post()  { curl -s -X POST --data-binary "$1" "http://127.0.0.1:$SVC_PORT/api/tx"; }

dump_logs() {
    echo "!!! slipstream-e2e FAILED — recent logs:" >&2
    for f in "$REGTEST_DIR"/logs/*.log "$SVC_LOG"; do
        [ -f "$f" ] || continue
        echo "--- tail $f" >&2
        tail -40 "$f" >&2
    done
}

cleanup() {
    [ -n "$SVC_PID" ] && kill "$SVC_PID" 2>/dev/null || true
    REGTEST_BIN_DIR="$BIN" "$ROOT/scripts/regtest/stop.sh" || true
    rm -rf "$LOCK"
}

mkdir -p "$(dirname "$REGTEST_DIR")"
LOCK="$REGTEST_DIR.lock"
if ! mkdir "$LOCK" 2>/dev/null; then
    echo "FAIL: $LOCK exists — another run of this suite is active." >&2
    echo "  REGTEST_DIR=$REGTEST_DIR scripts/regtest/stop.sh && rm -rf $LOCK" >&2
    exit 1
fi
trap 'code=$?; [ "$code" -ne 0 ] && dump_logs; cleanup; exit $code' EXIT
trap 'exit 130' INT TERM

for dep in sqlite3 jq node nc curl python3; do
    command -v "$dep" >/dev/null 2>&1 || { echo "$dep not installed" >&2; exit 1; }
done

PICKED=""
pick_port() {
    local p
    while :; do
        p=$(( (RANDOM % 20000) + 20001 ))
        [[ " $PICKED " == *" $p "* ]] && continue
        nc -z 127.0.0.1 "$p" 2>/dev/null && continue
        PICKED="$PICKED $p"
        printf -v "$1" '%s' "$p"
        return
    done
}

stage "allocate stack ports"
pick_port REGTEST_BITCOIND_RPC_PORT
pick_port REGTEST_BITCOIND_ZMQ_PORT
pick_port REGTEST_ENFORCER_RPC_PORT
pick_port REGTEST_ENFORCER_GRPC_PORT
pick_port SVC_PORT
export REGTEST_BITCOIND_RPC_PORT REGTEST_BITCOIND_ZMQ_PORT \
       REGTEST_ENFORCER_RPC_PORT REGTEST_ENFORCER_GRPC_PORT

stage "wipe data dir (fresh chain every run)"
rm -rf "$REGTEST_DIR/data" "$REGTEST_DIR/logs" "$REGTEST_DIR/run" "$BIN"
rm -f "$SVC_DB" "$SVC_DB-wal" "$SVC_DB-shm" "$SVC_LOG"

stage "binaries: cached bitcoind, slipstream enforcer"
REGTEST_BIN_DIR="$CACHE_BIN_DIR" "$ROOT/scripts/regtest/setup.sh" >/dev/null
mkdir -p "$BIN"
ln -sf "$CACHE_BIN_DIR/bitcoind"    "$BIN/bitcoind"
ln -sf "$CACHE_BIN_DIR/bitcoin-cli" "$BIN/bitcoin-cli"
ln -sf "$SLIPSTREAM_ENFORCER_BIN"   "$BIN/bip300301_enforcer"
"$BIN/bip300301_enforcer" --help | grep -q -- '--enable-slipstream' \
    || fail "$SLIPSTREAM_ENFORCER_BIN has no --enable-slipstream"

stage "start bitcoind-patched + walletless enforcer with slipstream"
REGTEST_BIN_DIR="$BIN" "$ROOT/scripts/regtest/start.sh" >/dev/null

stage "mature coins for the node wallet"
MINER_ADDR=$(wcli getnewaddress)
cli generatetoaddress 110 "$MINER_ADDR" >/dev/null

gbt() {
    curl -s --data-binary \
        '{"jsonrpc":"2.0","id":"t","method":"getblocktemplate","params":[{"rules":["segwit"],"capabilities":["coinbasetxn"]}]}' \
        -H 'content-type: application/json' "http://127.0.0.1:$REGTEST_ENFORCER_RPC_PORT"
}
wait_template_on_tip() {
    local tip
    tip=$(cli getbestblockhash)
    for _ in $(seq 1 60); do
        [ "$(gbt | jq -r '.result.previousblockhash // empty')" = "$tip" ] && return
        sleep 1
    done
    fail "the enforcer's template never reached tip $tip"
}
wait_template_on_tip

stage "start the slipstream service"
( cd "$ROOT/slipstream" && [ -d node_modules ] || npm ci --silent --no-audit --no-fund )
(
    cd "$ROOT/slipstream"
    ENFORCER_GBT_URL="http://127.0.0.1:$REGTEST_ENFORCER_RPC_PORT" \
    BITCOIND_RPC_URL="http://127.0.0.1:$REGTEST_BITCOIND_RPC_PORT" \
    BITCOIND_RPC_USER=user BITCOIND_RPC_PASS=password \
    SLIPSTREAM_DB_PATH="$SVC_DB" \
    PROXY_DB_PATH="$REGTEST_DIR/no-proxy.db" \
    SLIPSTREAM_PORT="$SVC_PORT" \
    SLIPSTREAM_POLL_MS=500 \
    SLIPSTREAM_CONFIRMATIONS=3 \
    PUBLIC_SLIPSTREAM_URL="http://127.0.0.1:$SVC_PORT" \
    exec node index.js
) > "$SVC_LOG" 2>&1 &
SVC_PID=$!
for _ in $(seq 1 30); do api /healthz >/dev/null 2>&1 && break; sleep 1; done
api /healthz >/dev/null || fail "the service never became healthy"

# A spend of one wallet UTXO at `fee_rate` sat/vB paying `extra` as a second
# output. A 1-sat `extra` is dust, which the node refuses to relay.
build_tx() {
    local fee_sats="$1" extra_btc="$2"
    local utxo txid vout amount pay dust raw
    utxo=$(wcli listunspent 100 | jq -c '[.[] | select(.amount >= 1)][0]')
    txid=$(jq -r .txid <<<"$utxo"); vout=$(jq -r .vout <<<"$utxo")
    amount=$(jq -r .amount <<<"$utxo")
    wcli lockunspent false "[{\"txid\":\"$txid\",\"vout\":$vout}]" >/dev/null
    pay=$(wcli getnewaddress); dust=$(wcli getnewaddress)
    local send
    send=$(python3 -c "print(f'{$amount - $fee_sats/1e8 - $extra_btc:.8f}')")
    raw=$(wcli createrawtransaction "[{\"txid\":\"$txid\",\"vout\":$vout}]" \
        "[{\"$pay\":$send},{\"$dust\":$extra_btc}]")
    wcli signrawtransactionwithwallet "$raw" | jq -r .hex
}

stage "1. a non-standard tx is accepted, and the node never sees it"
# ~141 vB for a P2WPKH one-in, two-out: 1_000 sat is ~7 sat/vB
TX_HEX=$(build_tx 1000 0.00000001)
[ "$(cli testmempoolaccept "[\"$TX_HEX\"]" | jq -r '.[0].allowed')" = "false" ] \
    || fail "the fixture tx is supposed to be refused by the node's policy"
RESP=$(post "$TX_HEX")
echo "  $RESP"
[ "$(jq -r .accepted <<<"$RESP")" = "true" ] || fail "slipstream refused the tx: $RESP"
TXID=$(jq -r .txid <<<"$RESP")
cli getrawmempool | jq -e --arg t "$TXID" 'index($t) == null' >/dev/null \
    || fail "the slipstream tx reached the node's mempool"

stage "2. it reaches the template"
for _ in $(seq 1 30); do
    [ "$(api "/api/tx/$TXID" | jq -r .tx.status)" = "in_template" ] && break
    sleep 1
done
[ "$(api "/api/tx/$TXID" | jq -r .tx.status)" = "in_template" ] \
    || fail "never seen in the template: $(api "/api/tx/$TXID")"
gbt | jq -e --arg t "$TXID" '[.result.transactions[].txid] | index($t) != null' >/dev/null \
    || fail "not in the enforcer's template"

stage "3. mining the template confirms it"
TEMPLATE_TXS=$(gbt | jq -c '[.result.transactions[].data]')
BLOCK=$(cli generateblock "$MINER_ADDR" "$TEMPLATE_TXS" | jq -r .hash)
echo "  mined $BLOCK"
for _ in $(seq 1 30); do
    [ "$(api "/api/tx/$TXID" | jq -r .tx.status)" = "mined" ] && break
    sleep 1
done
TX_JSON=$(api "/api/tx/$TXID")
[ "$(jq -r .tx.status <<<"$TX_JSON")" = "mined" ] || fail "never seen mined: $TX_JSON"
[ "$(jq -r .tx.mined_block_hash <<<"$TX_JSON")" = "$BLOCK" ] || fail "wrong block: $TX_JSON"
cli generatetoaddress 2 "$MINER_ADDR" >/dev/null
for _ in $(seq 1 30); do
    [ "$(api "/api/tx/$TXID" | jq -r .tx.status)" = "confirmed" ] && break
    sleep 1
done
TX_JSON=$(api "/api/tx/$TXID")
[ "$(jq -r .tx.status <<<"$TX_JSON")" = "confirmed" ] || fail "never confirmed: $TX_JSON"
jq -r '.events[].event' <<<"$TX_JSON" | tr '\n' ' '; echo
# A poll can land between the block connecting and the enforcer reporting
# it, and see the tx out of the template but not yet mined: a `pending` in
# between is correct, so it is not part of what is asserted.
[ "$(jq -r '[.events[].event | select(. != "pending")] | join(",")' <<<"$TX_JSON")" \
    = "accepted,in_template,mined,confirmed" ] || fail "unexpected history: $TX_JSON"

stage "4. under the fee floor: refused, and not left in the enforcer"
wait_template_on_tip
# 20 sat for ~141 vB is ~0.14 sat/vB: over the node's relay floor, under ours
LOW_HEX=$(build_tx 20 0.00001000)
RESP=$(post "$LOW_HEX")
echo "  $RESP"
[ "$(jq -r .reject_reason <<<"$RESP")" = "fee-rate-too-low" ] || fail "low fee not refused: $RESP"
LOW_TXID=$(jq -r .txid <<<"$RESP")
gbt | jq -e --arg t "$LOW_TXID" '[.result.transactions[].txid] | index($t) == null' >/dev/null \
    || fail "the refused tx is still in the enforcer's template"

stage "5. every submission is kept"
SUBS=$(sqlite3 "$SVC_DB" "SELECT accepted || ':' || COALESCE(reject_reason, '') FROM slipstream_submissions ORDER BY id" | tr '\n' ' ')
echo "  $SUBS"
[ "$SUBS" = "1: 0:fee-rate-too-low " ] || fail "unexpected submission log: $SUBS"

stage "6. info.json"
INFO=$(api /info.json)
[ "$(jq -r .software <<<"$INFO")" = "simplepool" ] || fail "info.json: $INFO"
[ "$(jq -r .slipstream_url <<<"$INFO")" = "http://127.0.0.1:$SVC_PORT" ] || fail "info.json: $INFO"
api /api/fees | jq -c .

echo
echo "=== slipstream-e2e: PASS"
