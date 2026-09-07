#!/usr/bin/env bash
# End-to-end test of the pplns-btc payout rail: real money, on L1, through a
# real bip300301_enforcer wallet.
#
#   bitcoind-patched  <--RPC/ZMQ--  bip300301_enforcer (--enable-wallet)
#          ^                                 ^
#          | the transaction lands here      | WalletService/SendTransaction
#          |                                 |
#          +---------------------------  payout worker (PAYOUT_RAIL=btc)
#
# tests/test_payout_regtest.sh already walks this sequence for the Thunder
# rail. This is the other one, and until now it had no coverage beyond unit
# tests against a stubbed client: nothing had ever asked a real enforcer
# wallet to build, sign and broadcast a payment, or checked that the sats
# arrived at the address a miner authorized with.
#
# The rail is the whole difference between the two pplns modes, and it is
# the half that moves money. tests/test_pplns_regtest.sh proves both modes
# credit pps_credits correctly; it stops exactly where this starts.
#
# What is asserted, in the order it has to happen:
#
#   1. a tick BROADCASTS and credits nobody. paid means mined, not sent, so
#      a transaction that exists is not yet a payment.
#   2. a tick before confirmation neither credits nor re-broadcasts. This is
#      the double-spend guard: the batch stays in flight and the tick blocks
#      on it.
#   3. one L1 block later a tick SETTLES, the ledger row appears, paid_sats
#      moves exactly once, and the in-flight row is gone.
#   4. the worker's address actually holds the sats on chain, read straight
#      out of the UTXO set rather than from anything the worker wrote.
#
# (4) is the assertion that cannot be faked by a bookkeeping bug: every
# other check reads a database the payout worker itself wrote.
#
# No Thunder here -- pplns-btc pays on L1 and never touches a sidechain, so
# the stack is bitcoind plus a wallet-enabled enforcer and nothing else.
#
# Env:
#   REGTEST_DIR      data dir, WIPED each run (default: <repo>/.regtest-btcpay)
#   REGTEST_BIN_DIR  binary cache, kept across runs (default: <repo>/.regtest/bin)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
export REGTEST_DIR="${REGTEST_DIR:-$ROOT/.regtest-btcpay}"
export REGTEST_BIN_DIR="${REGTEST_BIN_DIR:-$ROOT/.regtest/bin}"
# pplns-btc has no sidechain in it at all.
export REGTEST_SKIP_THUNDER=1

BIN="$REGTEST_BIN_DIR"
RPC="$ROOT/scripts/enforcer-rpc.sh"
PAYOUT_DB="/tmp/simplepool-btcpay-e2e.db"
# Mining sink; any valid regtest address works.
JUNK_ADDR="bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"
# Where the miner is paid. Deliberately NOT an enforcer-wallet address: the
# point is that the money leaves the pool's wallet and arrives somewhere the
# pool does not control, which an address the wallet owns could not show.
WORKER_ADDR="bcrt1qzyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3lgth6c"
OWED_SATS=250000

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

cli()   { "$BIN/bitcoin-cli" -datadir="$REGTEST_DIR/data/bitcoind" -regtest \
          -rpcuser=user -rpcpassword=password "$@"; }
stage() { echo; echo "=== btcpay-e2e: $1"; }

dump_logs() {
    echo "!!! btcpay-e2e FAILED — recent logs:" >&2
    for f in "$REGTEST_DIR"/logs/*.log; do
        [ -f "$f" ] || continue
        echo "--- tail $f" >&2
        tail -40 "$f" >&2
    done
}

cleanup() {
    "$ROOT/scripts/regtest/stop.sh" || true
    rm -rf "$LOCK"
}

LOCK="$REGTEST_DIR.lock"
if ! mkdir "$LOCK" 2>/dev/null; then
    echo "FAIL: $LOCK exists — another run of this suite is active." >&2
    echo "If it crashed and left the lock behind, clear it with:" >&2
    echo "  REGTEST_DIR=$REGTEST_DIR scripts/regtest/stop.sh && rm -rf $LOCK" >&2
    exit 1
fi
trap 'code=$?; [ "$code" -ne 0 ] && dump_logs; cleanup; exit $code' EXIT
trap 'exit 130' INT TERM

for dep in sqlite3 jq node nc curl; do
    command -v "$dep" >/dev/null 2>&1 || { echo "$dep not installed" >&2; exit 1; }
done

stage "allocate stack ports"
pick_port REGTEST_BITCOIND_RPC_PORT
pick_port REGTEST_BITCOIND_ZMQ_PORT
pick_port REGTEST_ENFORCER_RPC_PORT
pick_port REGTEST_ENFORCER_GRPC_PORT
export REGTEST_BITCOIND_RPC_PORT REGTEST_BITCOIND_ZMQ_PORT \
       REGTEST_ENFORCER_RPC_PORT REGTEST_ENFORCER_GRPC_PORT
export ENFORCER_URL="http://127.0.0.1:$REGTEST_ENFORCER_GRPC_PORT"
# What the payout worker is handed. host:port, no scheme -- enforcer-rpc.js
# adds http:// when there is none.
ENFORCER_RPC_ADDR="127.0.0.1:$REGTEST_ENFORCER_GRPC_PORT"
echo "  bitcoind=$REGTEST_BITCOIND_RPC_PORT enforcer=$REGTEST_ENFORCER_RPC_PORT/$REGTEST_ENFORCER_GRPC_PORT"

stage "wipe data dir (fresh chain every run)"
rm -rf "$REGTEST_DIR/data" "$REGTEST_DIR/logs" "$REGTEST_DIR/run"

stage "download prebuilt binaries"
"$ROOT/scripts/regtest/setup.sh"

stage "install payout worker deps"
if [ ! -d "$ROOT/payout/node_modules/better-sqlite3" ]; then
    npm ci --prefix "$ROOT/payout" --no-audit --no-fund
fi

stage "start bitcoind-patched + enforcer (wallet enabled)"
"$ROOT/scripts/regtest/start.sh"

stage "fund the enforcer wallet"
# The pool holds no keys: pplns-btc pays by asking the enforcer's wallet to
# send, so that wallet is what has to have spendable coins.
ENF_ADDR="$("$RPC" cusf.mainchain.v1.WalletService/CreateNewAddress | jq -r .address)"
echo "  enforcer wallet address: $ENF_ADDR"
RPC_TIMEOUT=120 "$RPC" cusf.mainchain.v1.MiningService/GenerateToAddress \
    '{"blocks": 5, "address": "'"$ENF_ADDR"'"}' > /dev/null
# Past coinbase maturity (100), or there is nothing spendable to pay from.
RPC_TIMEOUT=300 "$RPC" cusf.mainchain.v1.MiningService/GenerateToAddress \
    '{"blocks": 100, "address": "'"$JUNK_ADDR"'"}' > /dev/null
BAL="$("$RPC" cusf.mainchain.v1.WalletService/GetBalance | jq -r '.confirmedSats // .confirmed_sats // 0')"
echo "  height=$(cli getblockcount) enforcer confirmed balance=$BAL sats"
[ "${BAL:-0}" -gt "$OWED_SATS" ] || {
    echo "FAIL: enforcer wallet has $BAL sats, needs more than $OWED_SATS" >&2; exit 1; }

stage "seed pool DB: one worker owed $OWED_SATS sats, payable on L1"
rm -f "$PAYOUT_DB" "$PAYOUT_DB-wal" "$PAYOUT_DB-shm"
NOW="$(date +%s)"
sqlite3 "$PAYOUT_DB" < "$ROOT/schema.sql"
sqlite3 "$PAYOUT_DB" "
    INSERT INTO workers (name, first_seen, last_seen, payout_address)
    VALUES ('${WORKER_ADDR}.rig1', $NOW, $NOW, '$WORKER_ADDR');
    INSERT INTO pps_credits (worker_id, accrued_sats, paid_sats, last_updated)
    VALUES (1, $OWED_SATS, 0, $NOW);
"

# PAYOUT_RAIL=btc is what selects this client, and with it the enforcer
# variables become the required set and the Thunder ones are not read at
# all -- a correctly configured L1 pool must not be refused for lacking
# THUNDER_RPC_URL. That this tick runs with none of them set is that check.
run_tick() {
    PAYOUT_DB_PATH="$PAYOUT_DB" \
    PAYOUT_RAIL=btc \
    ENFORCER_RPC_ADDR="$ENFORCER_RPC_ADDR" \
    PAYOUT_FEE_RATE_SAT_VB=2 \
    PAYOUT_MIN_SATS=10000 \
    node "$ROOT/payout/run-once.mjs"
}

paid_sats()  { sqlite3 "$PAYOUT_DB" "SELECT paid_sats FROM pps_credits WHERE worker_id = 1"; }
ledger_n()   { sqlite3 "$PAYOUT_DB" "SELECT count(*) FROM payouts"; }
inflight_n() { sqlite3 "$PAYOUT_DB" "SELECT count(*) FROM payouts_in_flight"; }

stage "payout tick 1: broadcast only"
RESULT="$(run_tick)" || { echo "FAIL: payout tick reported failures: $RESULT" >&2; exit 1; }
echo "  tick result: $RESULT"
[ "$(jq -r .broadcast <<< "$RESULT")" = "1" ] || {
    echo "FAIL: expected exactly 1 broadcast worker" >&2; exit 1; }
# paid means mined, not sent. A transaction that merely exists is not a
# payment, and crediting here is how a pool pays twice for one debt.
[ "$(jq -r .paid <<< "$RESULT")" = "0" ] || {
    echo "FAIL: a broadcast must not report a paid worker" >&2; exit 1; }

stage "assert the broadcast credited nobody"
TXID="$(sqlite3 "$PAYOUT_DB" "SELECT txid FROM payouts_in_flight WHERE worker_id = 1")"
echo "  txid=$TXID paid_sats=$(paid_sats) ledger_rows=$(ledger_n) in_flight=$(inflight_n)"
[ "${#TXID}" -eq 64 ]     || { echo "FAIL: bad in-flight txid '$TXID'" >&2; exit 1; }
[ "$(paid_sats)" = "0" ]  || { echo "FAIL: paid_sats moved on a broadcast" >&2; exit 1; }
[ "$(ledger_n)" = "0" ]   || { echo "FAIL: payouts ledger written before confirmation" >&2; exit 1; }
[ "$(inflight_n)" = "1" ] || { echo "FAIL: batch must stay in flight until mined" >&2; exit 1; }

stage "assert bitcoind holds the tx, unconfirmed"
# Straight from the node's mempool, not from the enforcer that sent it.
cli getrawtransaction "$TXID" true > /dev/null 2>&1 || {
    echo "FAIL: bitcoind does not know $TXID — it was never broadcast" >&2
    cli getrawmempool >&2 || true
    exit 1; }
cli getrawtransaction "$TXID" true | jq -e '.blockhash == null' > /dev/null || {
    echo "FAIL: $TXID is already confirmed; the sequence below tests nothing" >&2; exit 1; }
echo "  bitcoind has $TXID in its mempool, unconfirmed"

stage "a tick before confirmation must not credit or re-broadcast"
RESULT="$(run_tick)" || { echo "FAIL: payout tick reported failures: $RESULT" >&2; exit 1; }
echo "  tick result: $RESULT"
[ "$(jq -r .waiting_on <<< "$RESULT")" = "$TXID" ] || {
    echo "FAIL: expected the tick to wait on $TXID" >&2; exit 1; }
[ "$(paid_sats)" = "0" ]  || { echo "FAIL: credited before the tx was mined" >&2; exit 1; }
[ "$(inflight_n)" = "1" ] || { echo "FAIL: batch left flight before confirming" >&2; exit 1; }

stage "mine one L1 block, then settle"
# Unlike Thunder, Bitcoin needs no nudging to include a transaction -- the
# client's mine() is a deliberate no-op. One block is the whole difference
# between broadcast and paid.
for attempt in 1 2 3 4 5; do
    RPC_TIMEOUT=120 "$RPC" cusf.mainchain.v1.MiningService/GenerateToAddress \
        '{"blocks": 1, "address": "'"$JUNK_ADDR"'"}' > /dev/null
    RESULT="$(run_tick)" || { echo "FAIL: payout tick reported failures: $RESULT" >&2; exit 1; }
    echo "  attempt $attempt: $RESULT"
    [ "$(jq -r .settled <<< "$RESULT")" = "1" ] && break
    sleep 1
done
[ "$(jq -r .settled <<< "$RESULT")" = "1" ] || {
    echo "FAIL: payout never settled after 5 L1 blocks" >&2; exit 1; }

stage "assert the ledger settled"
LEDGER_TXID="$(sqlite3 "$PAYOUT_DB" "SELECT txid FROM payouts WHERE worker_id = 1")"
echo "  txid=$LEDGER_TXID paid_sats=$(paid_sats) in_flight=$(inflight_n)"
[ "$LEDGER_TXID" = "$TXID" ]      || { echo "FAIL: ledger txid '$LEDGER_TXID' != '$TXID'" >&2; exit 1; }
[ "$(paid_sats)" = "$OWED_SATS" ] || { echo "FAIL: paid_sats=$(paid_sats) != $OWED_SATS" >&2; exit 1; }
[ "$(inflight_n)" = "0" ]         || { echo "FAIL: $(inflight_n) in-flight rows left" >&2; exit 1; }

stage "assert the sats are really at the worker's address"
# The one check that reads neither the payout worker's database nor the
# wallet that sent the money: an unspent output of exactly OWED_SATS at the
# miner's own address, straight out of the chain's UTXO set. Every other
# assertion above would still pass if the ledger were being written without
# a payment behind it.
SCAN="$(cli scantxoutset start '["addr('"$WORKER_ADDR"')"]')"
FOUND_SATS="$(jq -r '[.unspents[].amount] | add // 0 | . * 100000000 | round' <<< "$SCAN")"
echo "  utxo set holds $FOUND_SATS sats at $WORKER_ADDR"
[ "$FOUND_SATS" = "$OWED_SATS" ] || {
    echo "FAIL: expected $OWED_SATS sats at $WORKER_ADDR, found $FOUND_SATS" >&2
    jq . <<< "$SCAN" >&2 || true
    exit 1; }

# A second settled tick must not pay again. paid_sats is the latch, and the
# failure it guards against leaves no trace in the amounts themselves.
stage "a further tick pays nothing more"
RESULT="$(run_tick)" || { echo "FAIL: payout tick reported failures: $RESULT" >&2; exit 1; }
echo "  tick result: $RESULT"
[ "$(paid_sats)" = "$OWED_SATS" ] || {
    echo "FAIL: paid_sats moved to $(paid_sats) on a tick with nothing owed" >&2; exit 1; }
[ "$(ledger_n)" = "1" ] || {
    echo "FAIL: $(ledger_n) ledger rows for one payment" >&2; exit 1; }

echo
echo "btcpay-e2e: PASS (pplns-btc paid a miner on L1 through the enforcer wallet)"
