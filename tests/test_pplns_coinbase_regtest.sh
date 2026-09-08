#!/usr/bin/env bash
# End-to-end test of pool_mode = pplns-coinbase: the window paid straight out
# of the coinbase of the block that produced it.
#
#   bitcoind-patched  <-ZMQ/RPC-  bip300301_enforcer (walletless)
#          ^                              | GBT
#          | submitblock                  v
#          +----------------------- simplepool (pplns-coinbase)
#                                         ^ stratum
#                                         |
#                                  cpuminer.js
#
# The other two pplns rails credit pps_credits and hand the money to a payout
# worker ~100 blocks later. This one has no ledger step at all: the payment IS
# the block. So what has to be proved is different, and only the chain can
# prove it —
#
#   1. the pool starts and mines with NO pool_btc_address configured. There is
#      no pool wallet in this mode; if one were needed the config would have
#      refused it, and if the coinbase quietly paid one anyway the assertions
#      below would find it.
#   2. the mined block's coinbase pays the miners in the window, read out of
#      the chain rather than out of anything simplepool wrote.
#   3. NO output pays an address the pool controls. That is the whole claim of
#      the mode and it is the one thing a bookkeeping bug cannot fake.
#   4. pps_credits stays empty. Nothing accrues off-chain because nothing is
#      owed off-chain — a balance here would mean the pool thinks it owes
#      money it already paid on-chain.
#
# Env:
#   REGTEST_DIR      data dir, WIPED each run (default: <repo>/.regtest-cbwin)
#   REGTEST_BIN_DIR  binary cache, kept across runs (default: <repo>/.regtest/bin)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
export REGTEST_DIR="${REGTEST_DIR:-$ROOT/.regtest-cbwin}"
export REGTEST_BIN_DIR="${REGTEST_BIN_DIR:-$ROOT/.regtest/bin}"
export REGTEST_SKIP_THUNDER=1
export REGTEST_WALLETLESS=1

BIN="$REGTEST_BIN_DIR"
POOL_BIN="$ROOT/build/simplepool"
POOL_CONF="/tmp/simplepool-cbwin.conf"
POOL_LOG="/tmp/simplepool-cbwin.log"
POOL_DB="/tmp/simplepool-cbwin.db"

# The operator's fee address. Deliberately the ONLY address the pool controls
# in this test, so an assertion that no pool-controlled output exists beyond
# the fee is meaningful.
OPERATOR_ADDR="bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"
# What the miner authorizes with, and therefore what the coinbase must pay.
MINER_ADDR="bcrt1qzyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3lgth6c"
POOL_PID=""

cli()   { "$BIN/bitcoin-cli" -datadir="$REGTEST_DIR/data/bitcoind" -regtest \
          -rpcuser=user -rpcpassword=password "$@"; }
stage() { echo; echo "=== cbwin-e2e: $1"; }

dump_logs() {
    echo "!!! cbwin-e2e FAILED — recent logs:" >&2
    for f in "$REGTEST_DIR"/logs/*.log "$POOL_LOG"; do
        [ -f "$f" ] || continue
        echo "--- tail $f" >&2
        tail -40 "$f" >&2
    done
}

cleanup() {
    [ -n "$POOL_PID" ] && kill "$POOL_PID" 2>/dev/null || true
    "$ROOT/scripts/regtest/stop.sh" || true
    rm -rf "$LOCK"
}

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
pick_port POOL_PORT
export REGTEST_BITCOIND_RPC_PORT REGTEST_BITCOIND_ZMQ_PORT \
       REGTEST_ENFORCER_RPC_PORT REGTEST_ENFORCER_GRPC_PORT
export ENFORCER_URL="http://127.0.0.1:$REGTEST_ENFORCER_GRPC_PORT"

stage "wipe data dir (fresh chain every run)"
rm -rf "$REGTEST_DIR/data" "$REGTEST_DIR/logs" "$REGTEST_DIR/run"

stage "build simplepool"
make -C "$ROOT" -j >/dev/null

stage "download prebuilt binaries"
"$ROOT/scripts/regtest/setup.sh"

stage "start bitcoind-patched + walletless enforcer"
"$ROOT/scripts/regtest/start.sh"

stage "activate sidechain #9 via enforcer-template mining"
# The coinbase is classic-shaped, so the sidechain is not what is under test.
# It is activated anyway so the enforcer's template still carries the BIP301
# commitment outputs the coinbase builder has to preserve around the window —
# mining against a template without them would exercise an easier case than
# production ever runs.
"$ROOT/scripts/regtest/activate-thunder.sh"

stage "start simplepool in pplns-coinbase mode"
# Note what is NOT here: pool_btc_address. There is no pool wallet in this
# mode, and the config refuses one.
rm -f "$POOL_DB" "$POOL_DB-wal" "$POOL_DB-shm"
cat > "$POOL_CONF" <<EOF
listen_addr = 127.0.0.1
listen_port = ${POOL_PORT}

bitcoind_url = http://127.0.0.1:${REGTEST_ENFORCER_RPC_PORT}
bitcoind_poll_interval_ms = 500

operator_address = ${OPERATOR_ADDR}
fee_bps = 100
coinbase_tag = /simplepool-cbwin/

pool_mode = pplns-coinbase
pplns_window_diff_multiple = 2.0

initial_diff = 0.0000001
vardiff_enabled = 0
max_submits_per_sec = 100

db_path = ${POOL_DB}
log_level = debug
EOF
"$POOL_BIN" "$POOL_CONF" > "$POOL_LOG" 2>&1 &
POOL_PID=$!
for _ in $(seq 1 20); do nc -z 127.0.0.1 "$POOL_PORT" 2>/dev/null && break; sleep 1; done
kill -0 "$POOL_PID" 2>/dev/null || { echo "simplepool died on startup" >&2; exit 1; }

stage "mine one block through stratum as ${MINER_ADDR}"
TIP_BEFORE=$(cli getblockcount)
node "$ROOT/scripts/regtest/cpuminer.js" --port "$POOL_PORT" --user "$MINER_ADDR" --timeout 180
TIP_AFTER=$(cli getblockcount)
echo "  height: $TIP_BEFORE -> $TIP_AFTER"
[ "$TIP_AFTER" -gt "$TIP_BEFORE" ] || {
    echo "FAIL: block submitted but the chain did not advance" >&2; exit 1; }

stage "assert the coinbase paid the window, on chain"
TIP="$(cli getbestblockhash)"
CB_TXID="$(cli getblock "$TIP" 2 | jq -r '.tx[0].txid')"
CB_JSON="$(cli getblock "$TIP" 2 | jq -c '.tx[0]')"
echo "  block $TIP coinbase $CB_TXID"
CB_JSON="$CB_JSON" MINER_ADDR="$MINER_ADDR" OPERATOR_ADDR="$OPERATOR_ADDR" python3 - <<'PY'
import json, os, sys

cb = json.loads(os.environ['CB_JSON'])
miner = os.environ['MINER_ADDR']
op    = os.environ['OPERATOR_ADDR']

paid = {}
op_returns = 0
for o in cb['vout']:
    spk = o['scriptPubKey']
    if spk.get('type') == 'nulldata':
        op_returns += 1
        continue
    addr = spk.get('address')
    if addr is None:
        print(f"FAIL: spendable output with no address: {spk.get('hex')}", file=sys.stderr)
        sys.exit(1)
    paid[addr] = paid.get(addr, 0) + round(o['value'] * 1e8)

print(f"  spendable outputs: {len(paid)}  op_returns(commitments): {op_returns}")
for a, v in sorted(paid.items(), key=lambda kv: -kv[1]):
    who = 'MINER' if a == miner else ('operator fee' if a == op else 'UNKNOWN')
    print(f"    {v:>14} sats -> {a}  ({who})")

# 1. the miner in the window is paid, directly, in this block
if miner not in paid:
    print(f"FAIL: the window's miner {miner} has no coinbase output", file=sys.stderr)
    sys.exit(1)

# 2. nothing is paid to an address that is neither the window nor the fee.
#    There is no pool wallet in this mode, so any third address is one.
unknown = [a for a in paid if a not in (miner, op)]
if unknown:
    print(f"FAIL: coinbase pays {unknown}, which is neither the window nor "
          f"the operator fee — the pool is holding the reward", file=sys.stderr)
    sys.exit(1)

# 3. the miner gets the bulk of it. fee_bps is 100, so the operator should
#    hold ~1% and the miner ~99% — a split the other way round would mean the
#    fee and the payout had been swapped.
mine_sats = paid[miner]
op_sats   = paid.get(op, 0)
total     = mine_sats + op_sats
if mine_sats <= op_sats:
    print(f"FAIL: miner got {mine_sats} and the operator {op_sats}", file=sys.stderr)
    sys.exit(1)
share = op_sats / total if total else 0
if share > 0.02:
    print(f"FAIL: operator holds {share:.3%} of the block, expected ~1%",
          file=sys.stderr)
    sys.exit(1)
print(f"  miner {mine_sats} sats, operator {op_sats} sats ({share:.2%})")
PY

stage "assert the FIRST block took the bootstrap path"
# Worth pinning explicitly, because it is the path that used to deadlock: a
# pool with no shares has no window, and refusing to render there meant no
# coinbase, so no share, so no window, forever.
# Deterministic, unlike the tip-watcher's own empty-window message: on a fast
# chain the first block can be found before the first tip change, so whether
# the watcher ever SEES an empty window is a race. The initial job always
# carries none, and always says so.
grep -q "the first job of a process carries no window" "$POOL_LOG" || {
    echo "FAIL: expected the first job to be announced as windowless" >&2
    exit 1; }
echo "  bootstrap path announced, as it must be on a pool with no shares"

stage "mine a SECOND block, now that a window exists"
# The first block proved bootstrap. This one proves the mode: shares exist
# now, so the job carries a real window and the coinbase is built from it
# rather than from the connection.
#
# Wait for the pool to publish a job at the NEW height first. Without this the
# second miner connects while the pool is still serving the height-N job — the
# tip watcher polls every 500ms — mines a SIBLING of the block just found, and
# submitblock answers "inconclusive" because it neither extends nor replaces
# the tip. The chain then reads N -> N and the stage fails for a reason that
# has nothing to do with the mode. CI caught exactly that; locally the timing
# happened to hide it.
NEXT_HEIGHT=$((TIP_AFTER + 1))
for _ in $(seq 1 40); do
    grep -q "new job: height=${NEXT_HEIGHT} " "$POOL_LOG" && break
    sleep 1
done
grep -q "new job: height=${NEXT_HEIGHT} " "$POOL_LOG" || {
    echo "FAIL: the pool never published a job at height ${NEXT_HEIGHT}" >&2
    exit 1; }
echo "  pool is serving height ${NEXT_HEIGHT}"

TIP_BEFORE2=$(cli getblockcount)
node "$ROOT/scripts/regtest/cpuminer.js" --port "$POOL_PORT" --user "$MINER_ADDR" --timeout 180
TIP_AFTER2=$(cli getblockcount)
echo "  height: $TIP_BEFORE2 -> $TIP_AFTER2"
[ "$TIP_AFTER2" -gt "$TIP_BEFORE2" ] || {
    echo "FAIL: second block was not mined" >&2; exit 1; }

grep -q "pplns-coinbase: window of" "$POOL_LOG" || {
    echo "FAIL: no template was ever built from a real window — every block" >&2
    echo "      took the bootstrap path, so the mode itself is unproven" >&2
    exit 1; }
echo "  window path taken: $(grep -o 'window of [0-9]* miner(s), [0-9.]* difficulty' "$POOL_LOG" | tail -1)"

stage "assert the second block's coinbase also paid the miner"
TIP2="$(cli getbestblockhash)"
CB_JSON2="$(cli getblock "$TIP2" 2 | jq -c '.tx[0]')"
CB_JSON="$CB_JSON2" MINER_ADDR="$MINER_ADDR" OPERATOR_ADDR="$OPERATOR_ADDR" python3 - <<'PY'
import json, os, sys
cb = json.loads(os.environ['CB_JSON'])
miner, op = os.environ['MINER_ADDR'], os.environ['OPERATOR_ADDR']
paid = {}
for o in cb['vout']:
    spk = o['scriptPubKey']
    if spk.get('type') == 'nulldata':
        continue
    paid[spk['address']] = paid.get(spk['address'], 0) + round(o['value'] * 1e8)
if miner not in paid:
    print(f"FAIL: window-built coinbase does not pay {miner}", file=sys.stderr)
    sys.exit(1)
unknown = [a for a in paid if a not in (miner, op)]
if unknown:
    print(f"FAIL: window-built coinbase pays {unknown}", file=sys.stderr)
    sys.exit(1)
print(f"  miner {paid[miner]} sats, operator {paid.get(op, 0)} sats")
PY

stage "assert nothing accrued off-chain"
# The payment was the block. A pps_credits row here would mean the pool
# believes it owes money it has already paid on-chain — the double-payment
# this mode exists to make impossible.
CREDITS="$(sqlite3 "$POOL_DB" "SELECT COALESCE(SUM(accrued_sats),0) FROM pps_credits")"
ROWS="$(sqlite3 "$POOL_DB" "SELECT COUNT(*) FROM pps_credits")"
echo "  pps_credits rows=$ROWS accrued=$CREDITS"
[ "$ROWS" = "0" ] && [ "$CREDITS" = "0" ] || {
    echo "FAIL: pplns-coinbase accrued $CREDITS sats off-chain across $ROWS row(s);" >&2
    echo "      the coinbase already paid the miners" >&2
    exit 1; }

stage "assert the block was recorded, and needs no distribution"
BLK_ROWS="$(sqlite3 "$POOL_DB" "SELECT COUNT(*) FROM blocks_found")"
echo "  blocks_found rows=$BLK_ROWS"
[ "$BLK_ROWS" -ge 1 ] || { echo "FAIL: the block was not recorded" >&2; exit 1; }

echo
echo "cbwin-e2e: PASS (the window was paid from the block's own coinbase,"
echo "                 and the pool never held the reward)"
