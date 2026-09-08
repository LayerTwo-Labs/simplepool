#!/usr/bin/env bash
# End-to-end proof of pool_mode = solo, against a real chain.
#
#   bitcoind-patched  <->  bip300301_enforcer  <->  simplepool (solo)
#                                                        |
#                                                   cpuminer.js
#
# Solo is the default mode and the one most operators actually run, and until
# now it was the only mode with no end-to-end test anywhere. tests/
# test_integration.sh looks like one but is not: it subscribes, authorizes,
# submits one deliberately bogus share and asserts a reject row. It never
# mines, so it cannot see the thing solo is: a coinbase paying the finder.
# It is also not in CI. So the mode with the fewest moving parts had the
# weakest evidence, which is backwards.
#
# What only a chain can prove, and what this asserts:
#
#   1. the coinbase of a block found in solo mode pays the MINER'S OWN
#      address, plus the operator fee, and nothing else. No pool address is
#      configured and none may appear.
#   2. the coinbase is rendered PER CONNECTION. This is the defining property
#      of solo and the one most at risk from work on the pooled modes, which
#      share conn_render_coinbase(). Two miners authorize with two different
#      addresses and mine a block each; each block must pay its own finder.
#      A regression that rendered one coinbase for everybody would still pass
#      a single-miner test.
#   3. the enforcer's BIP300/301 commitment OP_RETURNs survive in the
#      coinbase, so these blocks can still carry a sidechain. Solo builds its
#      coinbase from the server-provided coinbasetxn exactly as the pooled
#      modes do, so this is not free.
#   4. nothing is credited off-chain. Solo has no ledger: the coinbase is the
#      payment. A pps_credits row would mean a pooled mode's accrual path ran.
#
# Env:
#   REGTEST_DIR      data dir, WIPED each run (default: <repo>/.regtest-solo)
#   REGTEST_BIN_DIR  binary cache, kept across runs (default: <repo>/.regtest/bin)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
export REGTEST_DIR="${REGTEST_DIR:-$ROOT/.regtest-solo}"
export REGTEST_BIN_DIR="${REGTEST_BIN_DIR:-$ROOT/.regtest/bin}"
export REGTEST_SKIP_THUNDER=1
export REGTEST_WALLETLESS=1

BIN="$REGTEST_BIN_DIR"
POOL_BIN="$ROOT/build/simplepool"
POOL_CONF="/tmp/simplepool-solo.conf"
POOL_LOG="/tmp/simplepool-solo.log"
POOL_DB="/tmp/simplepool-solo.db"

# The operator's fee address — the only address the pool itself controls.
OPERATOR_ADDR="bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"
# Two miners, two distinct addresses. Distinct is the whole point: it is what
# makes "each coinbase pays its own finder" a claim rather than a tautology.
MINER_A="bcrt1qzyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3lgth6c"
MINER_B="bcrt1qyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zs4w3j0"
POOL_PID=""

cli()   { "$BIN/bitcoin-cli" -datadir="$REGTEST_DIR/data/bitcoind" -regtest \
          -rpcuser=user -rpcpassword=password "$@"; }
stage() { echo; echo "=== solo-e2e: $1"; }

dump_logs() {
    echo "!!! solo-e2e FAILED — recent logs:" >&2
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
# Solo does not need a sidechain. It is activated so the enforcer's template
# carries the BIP301 commitment outputs the coinbase builder must preserve —
# mining against a template without them would test an easier case than any
# production drivechain pool runs, and assertion 3 below would be vacuous.
"$ROOT/scripts/regtest/activate-thunder.sh"

stage "start simplepool in solo mode"
# Note what is NOT here: pool_btc_address, and pool_mode itself. Solo is the
# default, so this is also a test that the default has not drifted.
cat > "$POOL_CONF" <<EOF
listen_addr = 127.0.0.1
listen_port = ${POOL_PORT}

bitcoind_url = http://127.0.0.1:${REGTEST_ENFORCER_RPC_PORT}
bitcoind_poll_interval_ms = 500

operator_address = ${OPERATOR_ADDR}
fee_bps = 100
coinbase_tag = /simplepool-solo/

initial_diff = 0.0000001
vardiff_enabled = 0
max_submits_per_sec = 100

db_path = ${POOL_DB}
log_level = debug
EOF
# Wipe the ledger too, not just the chain. Without this the counts below are
# assertions about every previous run of this suite rather than about this
# one -- "at least 2 blocks" passes trivially on the second run, which is how
# a stale-state pass hides a real regression.
rm -f "$POOL_DB" "$POOL_DB-wal" "$POOL_DB-shm"
"$POOL_BIN" "$POOL_CONF" > "$POOL_LOG" 2>&1 &
POOL_PID=$!
for _ in $(seq 1 20); do nc -z 127.0.0.1 "$POOL_PORT" 2>/dev/null && break; sleep 1; done
kill -0 "$POOL_PID" 2>/dev/null || { echo "simplepool died on startup" >&2; exit 1; }

grep -q "pool_mode=solo\|mode: solo\|solo" "$POOL_LOG" || true

# Mine one block per miner, checking the coinbase each time. Wrapped in a
# function because the two runs assert exactly the same thing about different
# addresses -- which is the point.
assert_block_pays() {
    local who="$1" addr="$2"
    local before after tip cb_json

    # Wait for a job at the height we are about to mine, or the second miner
    # connects while the pool is still serving the previous height, mines a
    # SIBLING, and submitblock answers "inconclusive". The chain then reads
    # N -> N and the failure has nothing to do with solo.
    before=$(cli getblockcount)
    local next=$((before + 1))

    # How many OP_RETURNs the enforcer's own coinbase carries right now. The
    # count is not fixed -- BIP300/301 commitments come and go with sidechain
    # activity, and on a quiet chain the witness commitment is the only one --
    # so asserting a constant would either be vacuous or wrong depending on
    # the day. Asserting "the same number came out as went in" is neither.
    local base_ors
    base_ors=$(curl -s --data-binary \
        '{"jsonrpc":"2.0","id":"t","method":"getblocktemplate","params":[{"rules":["segwit"],"capabilities":["coinbasetxn"]}]}' \
        -H 'content-type: application/json' \
        "http://127.0.0.1:${REGTEST_ENFORCER_RPC_PORT}" \
      | jq -r '.result.coinbasetxn.data' | python3 "$HERE/count_op_returns.py")
    echo "  enforcer template carries $base_ors OP_RETURN(s)"
    for _ in $(seq 1 40); do
        grep -q "new job: height=${next} " "$POOL_LOG" && break
        sleep 1
    done
    grep -q "new job: height=${next} " "$POOL_LOG" || {
        echo "FAIL: the pool never published a job at height ${next}" >&2
        exit 1; }

    node "$ROOT/scripts/regtest/cpuminer.js" --port "$POOL_PORT" \
         --user "$addr" --timeout 180
    after=$(cli getblockcount)
    echo "  height: $before -> $after"
    [ "$after" -gt "$before" ] || {
        echo "FAIL: $who submitted a block but the chain did not advance" >&2
        exit 1; }

    tip="$(cli getbestblockhash)"
    cb_json="$(cli getblock "$tip" 2 | jq -c '.tx[0]')"
    echo "  block $tip"
    CB_JSON="$cb_json" WHO="$who" MINER_ADDR="$addr" BASE_ORS="$base_ors" \
    OPERATOR_ADDR="$OPERATOR_ADDR" OTHER_ADDR="$3" python3 - <<'PY'
import json, os, sys

cb    = json.loads(os.environ['CB_JSON'])
who   = os.environ['WHO']
miner = os.environ['MINER_ADDR']
other = os.environ['OTHER_ADDR']
op    = os.environ['OPERATOR_ADDR']

paid, op_returns = {}, 0
for o in cb['vout']:
    spk = o['scriptPubKey']
    if spk.get('type') == 'nulldata':
        op_returns += 1
        continue
    addr = spk.get('address')
    if addr is None:
        print(f"FAIL: spendable output with no address: {spk.get('hex')}",
              file=sys.stderr)
        sys.exit(1)
    paid[addr] = paid.get(addr, 0) + round(o['value'] * 1e8)

print(f"  spendable outputs: {len(paid)}  op_returns(commitments): {op_returns}")
for a, v in sorted(paid.items(), key=lambda kv: -kv[1]):
    tag = 'FINDER' if a == miner else ('operator fee' if a == op else 'UNKNOWN')
    print(f"    {v:>14} sats -> {a}  ({tag})")

# 1. solo pays the finder, in the block they found.
if miner not in paid:
    print(f"FAIL: {who} found the block but its coinbase does not pay {miner}",
          file=sys.stderr)
    sys.exit(1)

# 2. and pays NOBODY else but the fee. In particular not the other miner:
#    that is what a shared, non-per-connection coinbase would look like.
if other in paid:
    print(f"FAIL: {who}'s block also pays the OTHER miner {other} — the "
          f"coinbase is not being rendered per connection", file=sys.stderr)
    sys.exit(1)
unknown = [a for a in paid if a not in (miner, op)]
if unknown:
    print(f"FAIL: coinbase pays {unknown}; solo configures no pool address, "
          f"so there is nothing else it may pay", file=sys.stderr)
    sys.exit(1)

# 3. every commitment the enforcer put in its template survived. Solo builds
#    on the server-provided coinbasetxn, so this is not free: a builder that
#    dropped one would still produce a VALID block -- just one no sidechain
#    can be merge-mined into, which fails silently and only on a drivechain.
base = int(os.environ['BASE_ORS'])
if op_returns != base:
    print(f"FAIL: the template carried {base} OP_RETURN(s) and the mined "
          f"coinbase has {op_returns}; a commitment was dropped or invented",
          file=sys.stderr)
    sys.exit(1)

# 4. the split is the finder's, not the operator's.
mine_sats, op_sats = paid[miner], paid.get(op, 0)
total = mine_sats + op_sats
share = op_sats / total if total else 0
if mine_sats <= op_sats or share > 0.02:
    print(f"FAIL: finder got {mine_sats}, operator {op_sats} ({share:.3%}); "
          f"expected the operator to hold ~1%", file=sys.stderr)
    sys.exit(1)
print(f"  {who} {mine_sats} sats, operator {op_sats} sats ({share:.2%})")
PY
}

stage "miner A mines a block, and must be paid in it"
assert_block_pays "miner A" "$MINER_A" "$MINER_B"

stage "miner B mines a block, and must be paid in ITS OWN coinbase"
# The real assertion of this suite. If cb1/cb2 were rendered once per job
# instead of once per connection, this block would pay miner A -- and a
# single-miner test would never notice.
assert_block_pays "miner B" "$MINER_B" "$MINER_A"

stage "assert the pool ran as solo, by its own account"
# The config above sets no pool_mode at all, so this also pins the DEFAULT.
# If the default ever drifted to a pooled mode, every assertion above would
# still pass -- a pooled coinbase pays whoever the window says, and with one
# miner in the window that is the same address -- so the chain alone cannot
# tell us which mode produced these blocks. The identity line can.
grep -q "mode=solo" "$POOL_LOG" || {
    echo "FAIL: the pool did not report itself as solo; the default mode may" >&2
    echo "      have drifted, and the coinbase assertions above cannot see it" >&2
    grep -i "pool identity" "$POOL_LOG" >&2
    exit 1; }
echo "  $(grep -o 'pool identity: .*' "$POOL_LOG" | head -1)"

# And no pooled-mode machinery ran. Solo must never build a window, take the
# pplns bootstrap path, or distribute; any of these would mean mode selection
# leaked between rails.
#
# Matched on the message prefixes the code actually emits, NOT on the bare
# word "pplns": the startup banner prints the git branch, so a run from a
# branch named after this work matched itself and failed. A grep that can be
# tripped by the branch name is not testing the binary.
POOLED='pplns-coinbase: |pplns: |pplns distribution|window of [0-9]+ miner'
if grep -qE "$POOLED" "$POOL_LOG"; then
    echo "FAIL: pooled-mode code ran in solo:" >&2
    grep -E "$POOLED" "$POOL_LOG" | head -5 >&2
    exit 1
fi
echo "  no pplns or window code path was taken"

stage "assert solo credits nothing off-chain"
# Solo has no ledger: the coinbase IS the payment. A row here would mean a
# pooled mode's accrual path ran in a mode that must never accrue.
CREDITS="$(sqlite3 "$POOL_DB" "SELECT COALESCE(SUM(accrued_sats),0) FROM pps_credits")"
ROWS="$(sqlite3 "$POOL_DB" "SELECT COUNT(*) FROM pps_credits")"
echo "  pps_credits rows=$ROWS accrued=$CREDITS"
[ "$ROWS" = "0" ] && [ "$CREDITS" = "0" ] || {
    echo "FAIL: solo wrote $CREDITS sats across $ROWS ledger row(s)" >&2
    exit 1; }

stage "assert both blocks were recorded, and both miners exist"
BLK_ROWS="$(sqlite3 "$POOL_DB" "SELECT COUNT(*) FROM blocks_found")"
WORKERS="$(sqlite3 "$POOL_DB" "SELECT COUNT(*) FROM workers")"
echo "  blocks_found=$BLK_ROWS workers=$WORKERS"
# Exactly two, not "at least": this run mined exactly two blocks with exactly
# two miners, and a fresh DB means any other number is a real defect rather
# than history.
[ "$BLK_ROWS" = "2" ] || { echo "FAIL: expected exactly 2 blocks recorded, got $BLK_ROWS" >&2; exit 1; }
[ "$WORKERS" = "2" ] || { echo "FAIL: expected exactly 2 workers recorded, got $WORKERS" >&2; exit 1; }

echo
echo "solo-e2e: PASS (each miner was paid in the block it found, from its own"
echo "                per-connection coinbase, with the commitments intact)"
