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
#   5. run one payout tick (payout/run-once.mjs) against thunder's RPC
#   6. assert the at-most-once ledger settled: payouts row with a txid,
#      paid_sats bumped, no in-flight rows — and thunder knows the tx
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

cleanup() { "$ROOT/scripts/regtest/stop.sh" || true; }
trap 'code=$?; [ "$code" -ne 0 ] && dump_logs; cleanup; exit $code' EXIT

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

stage "run one payout tick"
RESULT="$(
    PAYOUT_DB_PATH="$PAYOUT_DB" \
    THUNDER_RPC_URL="$THUNDER_RPC_URL" \
    THUNDER_FROM_ADDRESS="$RESERVE_ADDR" \
    PAYOUT_MIN_SATS=10000 \
    node "$ROOT/payout/run-once.mjs"
)" || { echo "FAIL: payout tick reported failures: $RESULT" >&2; exit 1; }
echo "  tick result: $RESULT"
[ "$(jq -r .paid <<< "$RESULT")" = "1" ] || {
    echo "FAIL: expected exactly 1 paid worker" >&2; exit 1; }

stage "assert the ledger settled"
TXID="$(sqlite3 "$PAYOUT_DB" "SELECT txid FROM payouts WHERE worker_id = 1")"
PAID="$(sqlite3 "$PAYOUT_DB" "SELECT paid_sats FROM pps_credits WHERE worker_id = 1")"
INFLIGHT="$(sqlite3 "$PAYOUT_DB" "SELECT count(*) FROM payouts_in_flight")"
echo "  txid=$TXID paid_sats=$PAID in_flight=$INFLIGHT"
[ "${#TXID}" -eq 64 ]           || { echo "FAIL: bad txid '$TXID'" >&2; exit 1; }
[ "$PAID" = "$OWED_SATS" ]      || { echo "FAIL: paid_sats=$PAID != $OWED_SATS" >&2; exit 1; }
[ "$INFLIGHT" = "0" ]           || { echo "FAIL: $INFLIGHT in-flight rows left" >&2; exit 1; }

stage "assert thunder knows the tx"
curl -sS -H 'Content-Type: application/json' \
    -d '{"jsonrpc":"2.0","id":1,"method":"get_transaction","params":["'"$TXID"'"]}' \
    "$THUNDER_RPC_URL" | jq -e '.result != null' > /dev/null || {
    echo "FAIL: thunder get_transaction($TXID) returned null" >&2; exit 1; }

stage "confirm the payout in a thunder block, check the worker's UTXO"
mine_thunder_block || true
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
