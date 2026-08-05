#!/usr/bin/env bash
# End-to-end test of the Thunder payout path, one-shot for CI:
#
#   bitcoind-patched  <-ZMQ/RPC-  bip300301_enforcer (wallet ENABLED)
#          ^                              | gRPC
#          |                              v
#          |                          thunder (sidechain #9)
#          |                              ^ JSON-RPC
#          |                              |
#          +--- deposit tx          payout/run-once.mjs
#
# Ports are allocated per run (pick_port), so this coexists with a dev
# stack in .regtest/ and with the coinbase e2e.
#
# This is the coverage the coinbase-shape e2e deliberately skips: the
# payout worker actually broadcasting a Thunder transaction. Stages:
#
#   1. start bitcoind-patched, a wallet-enabled enforcer (deposits need
#      WalletService), and thunder
#   2. activate sidechain #9, init the Thunder wallet
#   3. fund the enforcer wallet with mature coins, then move 1 BTC into
#      Thunder via a real CreateDepositTransaction + BMM-mined thunder
#      block — the same flow CLASSIC_PAYOUTS.md prescribes to operators
#   4. seed a pool DB with one worker owed 250 000 sats
#   5. run a payout tick (payout/run-once.mjs) against thunder's RPC and
#      assert it BROADCASTS without crediting: the batch stays in flight
#      with its txid, paid_sats is untouched, the ledger is empty
#   6. run another tick with the tx still in the mempool and assert it
#      neither credits nor re-broadcasts
#   7. BMM-mine a thunder block, tick again, and assert the ledger now
#      settles: payouts row carrying the same txid, paid_sats bumped,
#      no in-flight rows, and the worker holding an exact-amount UTXO
#
# Steps 5-7 are the point: a payout is credited when it is MINED, not
# when it is sent, so broadcasting and crediting are separate ticks.
#
# Deterministic by construction: every run starts from a completely
# fresh data dir; binaries are cached in REGTEST_BIN_DIR across runs.
# The data dir defaults to .regtest-payout/ so it never collides with a
# local dev stack in .regtest/ or the coinbase e2e in .regtest-e2e/.
#
# Env:
#   REGTEST_DIR      data dir, WIPED each run (default: <repo>/.regtest-payout)
#   REGTEST_BIN_DIR  binary cache, kept across runs (default: <repo>/.regtest/bin)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
export REGTEST_DIR="${REGTEST_DIR:-$ROOT/.regtest-payout}"
export REGTEST_BIN_DIR="${REGTEST_BIN_DIR:-$ROOT/.regtest/bin}"

BIN="$REGTEST_BIN_DIR"
RPC="$ROOT/scripts/enforcer-rpc.sh"
PAYOUT_DB="/tmp/simplepool-payout-e2e.db"
# Any valid regtest address works as a mining sink; same one the other
# regtest scripts use.
JUNK_ADDR="bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"
OWED_SATS=250000
DEPOSIT_SATS=100000000

# Pick a free port and assign it to the named variable. Not $()-command
# substitution: PICKED must accumulate across calls so two picks can't
# return the same not-yet-bound port.
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

tcli()  { "$BIN/thunder-cli" --rpc-url="$THUNDER_RPC_URL" "$@"; }
stage() { echo; echo "=== payout-e2e: $1"; }

dump_logs() {
    echo "!!! payout-e2e FAILED — recent logs:" >&2
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

# One run per data dir: two runs of the SAME suite share REGTEST_DIR
# (pidfiles + chain state), so the second run's wipe pulls the rug from
# under the first, and the first's cleanup then kills the second's
# freshly started daemons via the recreated pidfiles. Different suites
# coexist fine (own dirs, dynamic ports); same-suite runs are excluded
# here. Take the lock BEFORE installing traps — a refused run must not
# stop the owner's stack on its way out.
LOCK="$REGTEST_DIR.lock"
if ! mkdir "$LOCK" 2>/dev/null; then
    echo "FAIL: $LOCK exists — another run of this suite is active." >&2
    echo "If it crashed and left the lock behind, clear it with:" >&2
    echo "  REGTEST_DIR=$REGTEST_DIR scripts/regtest/stop.sh && rm -rf $LOCK" >&2
    exit 1
fi

trap 'code=$?; [ "$code" -ne 0 ] && dump_logs; cleanup; exit $code' EXIT
# Chain INT/TERM into the EXIT trap — bash skips EXIT traps when killed
# by an unhandled signal, which is how Ctrl-C used to orphan the stack.
trap 'exit 130' INT TERM

stage "allocate stack ports"
# Dynamic per-run ports: this test can run alongside a dev stack (or
# the coinbase e2e) without cross-wiring or refusing to start.
pick_port REGTEST_BITCOIND_RPC_PORT
pick_port REGTEST_BITCOIND_ZMQ_PORT
pick_port REGTEST_ENFORCER_RPC_PORT
pick_port REGTEST_ENFORCER_GRPC_PORT
pick_port REGTEST_THUNDER_RPC_PORT
pick_port REGTEST_THUNDER_P2P_PORT
export REGTEST_BITCOIND_RPC_PORT REGTEST_BITCOIND_ZMQ_PORT \
       REGTEST_ENFORCER_RPC_PORT REGTEST_ENFORCER_GRPC_PORT \
       REGTEST_THUNDER_RPC_PORT REGTEST_THUNDER_P2P_PORT
export ENFORCER_URL="http://127.0.0.1:$REGTEST_ENFORCER_GRPC_PORT"
THUNDER_RPC_URL="http://127.0.0.1:$REGTEST_THUNDER_RPC_PORT"
echo "  bitcoind=$REGTEST_BITCOIND_RPC_PORT zmq=$REGTEST_BITCOIND_ZMQ_PORT" \
     "enforcer=$REGTEST_ENFORCER_RPC_PORT/$REGTEST_ENFORCER_GRPC_PORT" \
     "thunder=$REGTEST_THUNDER_RPC_PORT/$REGTEST_THUNDER_P2P_PORT"

stage "wipe payout-e2e data dir (fresh chain every run)"
rm -rf "$REGTEST_DIR/data" "$REGTEST_DIR/logs" "$REGTEST_DIR/run"

stage "download prebuilt binaries"
"$ROOT/scripts/regtest/setup.sh"

stage "install payout worker deps"
if [ ! -d "$ROOT/payout/node_modules/better-sqlite3" ]; then
    npm ci --prefix "$ROOT/payout" --no-audit --no-fund
fi

stage "start bitcoind-patched + enforcer (wallet enabled) + thunder"
"$ROOT/scripts/regtest/start.sh"

stage "activate sidechain #9"
"$ROOT/scripts/regtest/activate-thunder.sh"

stage "init thunder wallet"
"$ROOT/scripts/regtest/thunder-init.sh"
RESERVE_ADDR="$(tcli get-new-address)"
WORKER_ADDR="$(tcli get-new-address)"
echo "  reserve address: $RESERVE_ADDR"
echo "  worker  address: $WORKER_ADDR"

stage "fund the enforcer wallet (mature coins for the deposit tx)"
ENF_ADDR="$("$RPC" cusf.mainchain.v1.WalletService/CreateNewAddress | jq -r .address)"
echo "  enforcer wallet address: $ENF_ADDR"
RPC_TIMEOUT=120 "$RPC" cusf.mainchain.v1.MiningService/GenerateToAddress \
    '{"blocks": 5, "address": "'"$ENF_ADDR"'"}' > /dev/null
# Bury them past coinbase maturity (100) so CreateDepositTransaction can
# spend them.
RPC_TIMEOUT=300 "$RPC" cusf.mainchain.v1.MiningService/GenerateToAddress \
    '{"blocks": 100, "address": "'"$JUNK_ADDR"'"}' > /dev/null
echo "  height now: $("$BIN/bitcoin-cli" -datadir="$REGTEST_DIR/data/bitcoind" \
    -regtest -rpcuser=user -rpcpassword=password getblockcount)"

stage "deposit $DEPOSIT_SATS sats into thunder"
"$RPC" cusf.mainchain.v1.WalletService/CreateDepositTransaction \
    '{"sidechain_id": 9, "address": "'"$RESERVE_ADDR"'", "value_sats": '"$DEPOSIT_SATS"', "fee_sats": 10000}' \
    | jq -r '"  deposit txid: " + (.txid.hex // .txid | tostring)'
RPC_TIMEOUT=120 "$RPC" cusf.mainchain.v1.MiningService/GenerateToAddress \
    '{"blocks": 1, "address": "'"$JUNK_ADDR"'"}' > /dev/null

# BMM-mine one thunder block: thunder-cli mine parks a BMM request and
# waits for the next L1 block to carry it, so the L1 block has to be
# produced while the CLI is blocked. Deposits credit once thunder
# processes the deposit's L1 block, which this also forces.
mine_thunder_block() {
    local before after
    before="$(tcli get-blockcount)"
    tcli mine --fee-sats 1000 >/dev/null 2>&1 &
    local mine_pid=$!
    sleep 2
    RPC_TIMEOUT=120 "$RPC" cusf.mainchain.v1.MiningService/GenerateToAddress \
        '{"blocks": 1, "address": "'"$JUNK_ADDR"'"}' > /dev/null
    wait "$mine_pid" || true
    after="$(tcli get-blockcount)"
    [ "$after" -gt "$before" ]
}

stage "BMM-mine a thunder block + wait for the deposit to credit"
for attempt in 1 2 3 4 5; do
    mine_thunder_block || echo "  BMM attempt $attempt missed, retrying"
    BAL="$(tcli balance | jq -r .available_sats)"
    echo "  thunder height=$(tcli get-blockcount) available=$BAL sats"
    [ "$BAL" -ge "$DEPOSIT_SATS" ] && break
    sleep 1
done
[ "$BAL" -ge "$DEPOSIT_SATS" ] || {
    echo "FAIL: deposit never credited (available=$BAL)" >&2; exit 1; }

stage "seed pool DB: one worker owed $OWED_SATS sats"
rm -f "$PAYOUT_DB"
NOW="$(date +%s)"
sqlite3 "$PAYOUT_DB" < "$ROOT/schema.sql"
sqlite3 "$PAYOUT_DB" "
    INSERT INTO workers (name, first_seen, last_seen, payout_address)
    VALUES ('${WORKER_ADDR}.rig1', $NOW, $NOW, '$WORKER_ADDR');
    INSERT INTO pps_credits (worker_id, accrued_sats, paid_sats, last_updated)
    VALUES (1, $OWED_SATS, 0, $NOW);
"

run_tick() {
    PAYOUT_DB_PATH="$PAYOUT_DB" \
    THUNDER_RPC_URL="$THUNDER_RPC_URL" \
    THUNDER_FROM_ADDRESS="$RESERVE_ADDR" \
    PAYOUT_MIN_SATS=10000 \
    node "$ROOT/payout/run-once.mjs"
}

paid_sats()  { sqlite3 "$PAYOUT_DB" "SELECT paid_sats FROM pps_credits WHERE worker_id = 1"; }
ledger_n()   { sqlite3 "$PAYOUT_DB" "SELECT count(*) FROM payouts"; }
inflight_n() { sqlite3 "$PAYOUT_DB" "SELECT count(*) FROM payouts_in_flight"; }

# A payout is credited when it is MINED, not when it is sent — so the tick
# that broadcasts and the tick that credits are two different ticks, with a
# thunder block in between. Everything below walks that sequence.

stage "payout tick 1: broadcast only"
RESULT="$(run_tick)" || { echo "FAIL: payout tick reported failures: $RESULT" >&2; exit 1; }
echo "  tick result: $RESULT"
[ "$(jq -r .broadcast <<< "$RESULT")" = "1" ] || {
    echo "FAIL: expected exactly 1 broadcast worker" >&2; exit 1; }
[ "$(jq -r .paid <<< "$RESULT")" = "0" ] || {
    echo "FAIL: a broadcast must not report a paid worker" >&2; exit 1; }

stage "assert the broadcast credited nobody"
TXID="$(sqlite3 "$PAYOUT_DB" "SELECT txid FROM payouts_in_flight WHERE worker_id = 1")"
echo "  txid=$TXID paid_sats=$(paid_sats) ledger_rows=$(ledger_n) in_flight=$(inflight_n)"
[ "${#TXID}" -eq 64 ]     || { echo "FAIL: bad in-flight txid '$TXID'" >&2; exit 1; }
[ "$(paid_sats)" = "0" ]  || { echo "FAIL: paid_sats moved on a broadcast" >&2; exit 1; }
[ "$(ledger_n)" = "0" ]   || { echo "FAIL: payouts ledger written before confirmation" >&2; exit 1; }
[ "$(inflight_n)" = "1" ] || { echo "FAIL: batch must stay in flight until mined" >&2; exit 1; }

stage "assert thunder has the tx, unconfirmed"
curl -sS -H 'Content-Type: application/json' \
    -d '{"jsonrpc":"2.0","id":1,"method":"get_transaction","params":["'"$TXID"'"]}' \
    "$THUNDER_RPC_URL" | jq -e '.result != null and .result.block_hash == null' > /dev/null || {
    echo "FAIL: thunder should hold $TXID in its mempool, unconfirmed" >&2; exit 1; }

stage "a tick before confirmation must not credit or re-broadcast"
RESULT="$(run_tick)" || { echo "FAIL: payout tick reported failures: $RESULT" >&2; exit 1; }
echo "  tick result: $RESULT"
[ "$(jq -r .waiting_on <<< "$RESULT")" = "$TXID" ] || {
    echo "FAIL: expected the tick to wait on $TXID" >&2; exit 1; }
[ "$(paid_sats)" = "0" ]  || { echo "FAIL: credited before the tx was mined" >&2; exit 1; }
[ "$(inflight_n)" = "1" ] || { echo "FAIL: batch left flight before confirming" >&2; exit 1; }

stage "mine a thunder block, then settle"
for attempt in 1 2 3 4 5; do
    mine_thunder_block || echo "  BMM attempt $attempt missed, retrying"
    RESULT="$(run_tick)" || { echo "FAIL: payout tick reported failures: $RESULT" >&2; exit 1; }
    echo "  attempt $attempt: $RESULT"
    [ "$(jq -r .settled <<< "$RESULT")" = "1" ] && break
    sleep 1
done
[ "$(jq -r .settled <<< "$RESULT")" = "1" ] || {
    echo "FAIL: payout never settled after 5 thunder blocks" >&2; exit 1; }

stage "assert the ledger settled"
LEDGER_TXID="$(sqlite3 "$PAYOUT_DB" "SELECT txid FROM payouts WHERE worker_id = 1")"
echo "  txid=$LEDGER_TXID paid_sats=$(paid_sats) in_flight=$(inflight_n)"
[ "$LEDGER_TXID" = "$TXID" ]        || { echo "FAIL: ledger txid '$LEDGER_TXID' != '$TXID'" >&2; exit 1; }
[ "$(paid_sats)" = "$OWED_SATS" ]   || { echo "FAIL: paid_sats=$(paid_sats) != $OWED_SATS" >&2; exit 1; }
[ "$(inflight_n)" = "0" ]           || { echo "FAIL: $(inflight_n) in-flight rows left" >&2; exit 1; }

stage "check the worker's UTXO"
# Exact-amount UTXO at the worker address after a mined block is the
# end-to-end proof the transfer applied. (Wallet-total math is useless
# here: this wallet is also the thunder block producer, so the tx fee
# comes straight back to it via the block it mines.)
tcli get-wallet-utxos | jq -e \
    --arg addr "$WORKER_ADDR" --argjson want "$OWED_SATS" \
    '[.[] | select(.output.address == $addr and .output.content.Value == $want)] | length >= 1' \
    > /dev/null || {
    echo "FAIL: no ${OWED_SATS}-sat UTXO at worker address $WORKER_ADDR" >&2
    tcli get-wallet-utxos | jq . >&2 || true
    exit 1
}
echo "  worker UTXO of $OWED_SATS sats confirmed"

echo
echo "payout-e2e: PASS"
