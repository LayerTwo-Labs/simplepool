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
#   2. all three due workers leave in ONE transaction, and the two sharing a
#      payout address get ONE output carrying the SUM of both debts. Read off
#      the wire before it is mined.
#   3. a tick before confirmation neither credits nor re-broadcasts. This is
#      the double-spend guard: the batch stays in flight and the tick blocks
#      on it.
#   4. one L1 block later a tick SETTLES, a ledger row appears per worker
#      against the one txid, each rig is credited its OWN debt, and the
#      in-flight rows are gone.
#   5. the miners' addresses actually hold the sats on chain, read straight
#      out of the UTXO set rather than from anything the worker wrote.
#
# (5) is the assertion that cannot be faked by a bookkeeping bug: every
# other check reads a database the payout worker itself wrote.
#
# Three workers rather than one because a single recipient never builds a
# multi-destination transaction, which is what every real pool sends, and
# never exercises the address merge at all.
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
# Where the miners are paid. Deliberately NOT enforcer-wallet addresses: the
# point is that the money leaves the pool's wallet and arrives somewhere the
# pool does not control, which an address the wallet owns could not show.
#
# Two addresses, three workers: rig2 authorizes with the same address as
# rig1, which is ordinary (one miner, two machines) and is the case the
# destinations map has to merge rather than overwrite.
ADDR_A="bcrt1qzyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3lgth6c"
ADDR_B="bcrt1qyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zs4w3j0"
OWED_A=250000
OWED_B1=180000
OWED_B2=120000
# Distinct per worker and distinct in sum, so no assertion below can pass by
# coincidence: 180000+120000 = 300000, which is neither operand.
OWED_SATS=$((OWED_A + OWED_B1 + OWED_B2))

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

stage "seed pool DB: three workers, two of them sharing one payout address"
# The production-normal case, and the one the single-recipient run could not
# reach. Two things only a multi-recipient batch can show:
#
#   1. every due worker leaves in ONE transaction, not one each. A payout per
#      worker is a fee per worker, and it breaks the one-tx-per-batch
#      invariant the whole at-most-once design rests on.
#   2. two rigs authorized with the SAME payout address are SUMMED. The
#      destinations map is keyed by address, so an assignment instead of a
#      sum pays that miner once for two debts while the ledger marks both
#      settled -- a shortfall that balances perfectly on the pool's side and
#      is visible only to the miner. groupByAddress in payout.js merges them
#      and the client sums again; this proves the pair against a real wallet
#      rather than against a stub.
rm -f "$PAYOUT_DB" "$PAYOUT_DB-wal" "$PAYOUT_DB-shm"
NOW="$(date +%s)"
sqlite3 "$PAYOUT_DB" < "$ROOT/schema.sql"
sqlite3 "$PAYOUT_DB" "
    INSERT INTO workers (id, name, first_seen, last_seen, payout_address) VALUES
      (1, '${ADDR_A}.rig1', $NOW, $NOW, '$ADDR_A'),
      (2, '${ADDR_B}.rig1', $NOW, $NOW, '$ADDR_B'),
      (3, '${ADDR_B}.rig2', $NOW, $NOW, '$ADDR_B');
    INSERT INTO pps_credits (worker_id, accrued_sats, paid_sats, last_updated) VALUES
      (1, $OWED_A,  0, $NOW),
      (2, $OWED_B1, 0, $NOW),
      (3, $OWED_B2, 0, $NOW);
"
echo "  worker 1 -> $ADDR_A  $OWED_A sats"
echo "  worker 2 -> $ADDR_B  $OWED_B1 sats"
echo "  worker 3 -> $ADDR_B  $OWED_B2 sats  (same address as worker 2)"
echo "  expected on chain: $OWED_A at A, $((OWED_B1 + OWED_B2)) at B"

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

paid_of()    { sqlite3 "$PAYOUT_DB" "SELECT paid_sats FROM pps_credits WHERE worker_id = $1"; }
paid_total() { sqlite3 "$PAYOUT_DB" "SELECT COALESCE(SUM(paid_sats),0) FROM pps_credits"; }
ledger_n()   { sqlite3 "$PAYOUT_DB" "SELECT count(*) FROM payouts"; }
inflight_n() { sqlite3 "$PAYOUT_DB" "SELECT count(*) FROM payouts_in_flight"; }
# Sats currently unspent at an address, straight out of the chain's UTXO set.
utxo_sats()  { cli scantxoutset start '["addr('"$1"')"]' \
               | jq -r '[.unspents[].amount] | add // 0 | . * 100000000 | round'; }

stage "payout tick 1: broadcast only"
RESULT="$(run_tick)" || { echo "FAIL: payout tick reported failures: $RESULT" >&2; exit 1; }
echo "  tick result: $RESULT"
[ "$(jq -r .broadcast <<< "$RESULT")" = "3" ] || {
    echo "FAIL: expected 3 broadcast workers, got $(jq -r .broadcast <<< "$RESULT")" >&2; exit 1; }
# paid means mined, not sent. A transaction that exists is not yet a payment.
[ "$(jq -r .paid <<< "$RESULT")" = "0" ] || {
    echo "FAIL: a broadcast must not report a paid worker" >&2; exit 1; }

stage "assert all three left in ONE transaction"
TXIDS="$(sqlite3 "$PAYOUT_DB" "SELECT DISTINCT txid FROM payouts_in_flight")"
TXID="$(head -1 <<< "$TXIDS")"
echo "  in_flight=$(inflight_n) distinct txids=$(wc -l <<< "$TXIDS" | tr -d ' ') txid=$TXID"
[ "$(inflight_n)" = "3" ] || { echo "FAIL: expected 3 in-flight rows" >&2; exit 1; }
[ "$(wc -l <<< "$TXIDS" | tr -d ' ')" = "1" ] || {
    echo "FAIL: the batch went out as more than one transaction:" >&2
    echo "$TXIDS" >&2; exit 1; }
[ "${#TXID}" -eq 64 ]     || { echo "FAIL: bad in-flight txid '$TXID'" >&2; exit 1; }
[ "$(paid_total)" = "0" ] || { echo "FAIL: paid_sats moved on a broadcast" >&2; exit 1; }
[ "$(ledger_n)" = "0" ]   || { echo "FAIL: payouts ledger written before confirmation" >&2; exit 1; }

stage "assert the transaction pays each address once, at the summed amount"
# Read straight off the wire, before it is mined: two miner outputs, not
# three. Three would mean the shared address was written twice; one at the
# wrong value would mean it was overwritten rather than summed.
RAW="$(cli getrawtransaction "$TXID" true)"
A_OUT="$(jq -r --arg a "$ADDR_A" '[.vout[] | select(.scriptPubKey.address == $a) | .value * 100000000 | round] | add // 0' <<< "$RAW")"
B_OUT="$(jq -r --arg b "$ADDR_B" '[.vout[] | select(.scriptPubKey.address == $b) | .value * 100000000 | round] | add // 0' <<< "$RAW")"
B_N="$(jq -r --arg b "$ADDR_B" '[.vout[] | select(.scriptPubKey.address == $b)] | length' <<< "$RAW")"
echo "  outputs: A=$A_OUT sats  B=$B_OUT sats across $B_N output(s)"
[ "$A_OUT" = "$OWED_A" ] || {
    echo "FAIL: worker A output is $A_OUT, expected $OWED_A" >&2; exit 1; }
[ "$B_OUT" = "$((OWED_B1 + OWED_B2))" ] || {
    echo "FAIL: the shared address got $B_OUT, expected $((OWED_B1 + OWED_B2))." >&2
    echo "      Two rigs on one address must be SUMMED, not overwritten — this" >&2
    echo "      is the shortfall that balances on the pool's side and is" >&2
    echo "      visible only to the miner." >&2
    exit 1; }
[ "$B_N" = "1" ] || {
    echo "FAIL: the shared address appears in $B_N outputs; the destinations" >&2
    echo "      map should have merged them into one" >&2; exit 1; }
jq -e '.blockhash == null' <<< "$RAW" > /dev/null || {
    echo "FAIL: $TXID is already confirmed; the sequence below tests nothing" >&2; exit 1; }

stage "a tick before confirmation must not credit or re-broadcast"
RESULT="$(run_tick)" || { echo "FAIL: payout tick reported failures: $RESULT" >&2; exit 1; }
echo "  tick result: $RESULT"
[ "$(jq -r .waiting_on <<< "$RESULT")" = "$TXID" ] || {
    echo "FAIL: expected the tick to wait on $TXID" >&2; exit 1; }
[ "$(paid_total)" = "0" ] || { echo "FAIL: credited before the tx was mined" >&2; exit 1; }
[ "$(inflight_n)" = "3" ] || { echo "FAIL: batch left flight before confirming" >&2; exit 1; }

stage "mine one L1 block, then settle"
# Unlike Thunder, Bitcoin needs no nudging to include a transaction -- the
# client's mine() is a deliberate no-op. One block is the whole difference
# between broadcast and paid.
for attempt in 1 2 3 4 5; do
    RPC_TIMEOUT=120 "$RPC" cusf.mainchain.v1.MiningService/GenerateToAddress \
        '{"blocks": 1, "address": "'"$JUNK_ADDR"'"}' > /dev/null
    RESULT="$(run_tick)" || { echo "FAIL: payout tick reported failures: $RESULT" >&2; exit 1; }
    echo "  attempt $attempt: $RESULT"
    [ "$(jq -r .settled <<< "$RESULT")" = "3" ] && break
    sleep 1
done
[ "$(jq -r .settled <<< "$RESULT")" = "3" ] || {
    echo "FAIL: expected 3 settled workers, got $(jq -r .settled <<< "$RESULT")" >&2; exit 1; }

stage "assert the ledger settled, per worker"
echo "  paid: w1=$(paid_of 1) w2=$(paid_of 2) w3=$(paid_of 3) ledger_rows=$(ledger_n) in_flight=$(inflight_n)"
[ "$(paid_of 1)" = "$OWED_A" ]  || { echo "FAIL: worker 1 paid_sats=$(paid_of 1) != $OWED_A" >&2; exit 1; }
# Each of the two rigs is credited its OWN debt, even though one transaction
# output covered both. Crediting the merged amount to either one would leave
# the other owed forever.
[ "$(paid_of 2)" = "$OWED_B1" ] || { echo "FAIL: worker 2 paid_sats=$(paid_of 2) != $OWED_B1" >&2; exit 1; }
[ "$(paid_of 3)" = "$OWED_B2" ] || { echo "FAIL: worker 3 paid_sats=$(paid_of 3) != $OWED_B2" >&2; exit 1; }
[ "$(ledger_n)" = "3" ]         || { echo "FAIL: expected 3 ledger rows, got $(ledger_n)" >&2; exit 1; }
[ "$(inflight_n)" = "0" ]       || { echo "FAIL: $(inflight_n) in-flight rows left" >&2; exit 1; }
LEDGER_TXIDS="$(sqlite3 "$PAYOUT_DB" "SELECT DISTINCT txid FROM payouts")"
[ "$LEDGER_TXIDS" = "$TXID" ] || {
    echo "FAIL: ledger txids '$LEDGER_TXIDS' != the one broadcast '$TXID'" >&2; exit 1; }

stage "assert the sats are really at the miners' addresses"
# The one check that reads neither the payout worker's database nor the
# wallet that sent the money, straight out of the chain's UTXO set. Every
# other assertion above would still pass if the ledger were being written
# without a payment behind it.
A_SATS="$(utxo_sats "$ADDR_A")"
B_SATS="$(utxo_sats "$ADDR_B")"
echo "  utxo set: $A_SATS at A, $B_SATS at B"
[ "$A_SATS" = "$OWED_A" ] || {
    echo "FAIL: expected $OWED_A sats at $ADDR_A, found $A_SATS" >&2; exit 1; }
[ "$B_SATS" = "$((OWED_B1 + OWED_B2))" ] || {
    echo "FAIL: expected $((OWED_B1 + OWED_B2)) sats at $ADDR_B, found $B_SATS" >&2; exit 1; }

stage "a further tick pays nothing more"
# paid_sats is the latch, and the failure it guards against leaves no trace
# in the amounts themselves.
RESULT="$(run_tick)" || { echo "FAIL: payout tick reported failures: $RESULT" >&2; exit 1; }
echo "  tick result: $RESULT"
[ "$(paid_total)" = "$((OWED_A + OWED_B1 + OWED_B2))" ] || {
    echo "FAIL: paid total moved to $(paid_total) on a tick with nothing owed" >&2; exit 1; }
[ "$(ledger_n)" = "3" ] || {
    echo "FAIL: $(ledger_n) ledger rows for three payments" >&2; exit 1; }

echo
echo "btcpay-e2e: PASS (pplns-btc paid three miners on L1 in one transaction,"
echo "                  with the shared address summed)"
