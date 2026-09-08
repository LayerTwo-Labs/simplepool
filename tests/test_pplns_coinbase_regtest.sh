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
#   4. NOTHING is owed off-chain, ever. pps_credits must be empty: this mode
#      writes no ledger row at all, so a row of any size means some other
#      rail's code path ran.
#   5. the policy is stated in the log. A claim below the payout floor is
#      forfeited to the operator and never settled, which is a trap unless
#      the operator can see it — so the disclosure lines are asserted here
#      exactly like the money is.
#   6. a MIXED window really does forfeit, on chain. Claims of 100 : 10 : 1,
#      a floor between the last two: the first two are paid in the coinbase,
#      the third gets no output, and its satoshis turn up on the operator's.
#
# That last stage is the one this file could not do for a long time, and the
# reason is worth writing down. Share difficulty is clamped to network
# difficulty on regtest, so a 2.0x window holds about two shares -- every
# other stage here reports "window of 1 miner(s)". A mixed window needs a much
# wider multiple AND a share history, so it seeds the shares table directly
# with the pool STOPPED. That is replaying the pool's own record of accepted
# work, not stubbing the thing under test: the window query, the split, the
# builder, the block and the outputs read back off the chain are all real.
#
# An earlier version of this file had a stage that squeezed the byte budget
# and printed how much had carried. It printed 0 every time and passed
# regardless, which is worse than no stage at all. This one asserts the
# amounts: 1 share in 111 of the payable reward, forfeited, and the operator
# holding strictly more than its fee.
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

stage "assert NOTHING is owed off-chain"
# The payment was the block, so there is no ledger at all in this mode: not
# for the miners the coinbase paid, and not for the ones it could not. A claim
# below the payout floor is forfeited to the operator outright — it is income,
# not a debt, and nothing records it.
#
# So this is unconditional, which is what makes it worth asserting. Any row
# here means some other rail's crediting path ran against a pplns-coinbase
# pool, which is the bug that would quietly recreate the custody this mode
# exists to remove.
CREDITS="$(sqlite3 "$POOL_DB" "SELECT COALESCE(SUM(accrued_sats),0) FROM pps_credits")"
ROWS="$(sqlite3 "$POOL_DB" "SELECT COUNT(*) FROM pps_credits")"
echo "  pps_credits rows=$ROWS accrued=$CREDITS"
[ "$ROWS" = "0" ] && [ "$CREDITS" = "0" ] || {
    echo "FAIL: pplns-coinbase wrote $CREDITS sats across $ROWS ledger row(s)." >&2
    echo "      This mode has no ledger: the block IS the payment." >&2
    exit 1; }

stage "assert the payout floor is disclosed, not silent"
# The floor decides who this pool refuses to pay, and a miner below it earns
# nothing however long it mines. That is a defensible policy and an
# indefensible surprise, so the operator has to be told twice: once at
# startup, and once per block with the actual numbers. If these lines ever
# regress the policy silently becomes a trap, which is why they are asserted
# here alongside the money.
grep -q "payout floor 546 sats" "$POOL_LOG" || {
    echo "FAIL: the pool never stated its payout floor at startup" >&2
    grep -i "floor" "$POOL_LOG" | head -5 >&2
    exit 1; }
grep -q "NOT PAID" "$POOL_LOG" || {
    echo "FAIL: the startup line does not say a miner below the floor is unpaid" >&2
    exit 1; }
echo "  startup: $(grep -o 'payout floor [0-9]* sats' "$POOL_LOG" | head -1)"

# And per block: this harness has one miner, who takes the whole window, so
# the expected line is the all-paid one. Asserting the all-paid wording rather
# than merely "some line was printed" is what keeps this from passing on a
# build where the reporting broke in the direction of saying nothing.
grep -q "paid all .* miner(s) in the window" "$POOL_LOG" || {
    echo "FAIL: no per-block payment line for a block that paid everyone" >&2
    grep -i "pplns-coinbase: block" "$POOL_LOG" | tail -5 >&2
    exit 1; }
echo "  per block: $(grep -o 'paid all [0-9]* miner(s) in the window [0-9]* sats' "$POOL_LOG" | tail -1)"

# The window-level warning is the one an operator can act on BEFORE a block
# makes it real. With everyone clearing the floor it must say so rather than
# say nothing — a line that only ever appears on the bad path is a line
# nobody notices is missing.
grep -q "every miner in the window clears the 546-sat payout floor" "$POOL_LOG" || {
    echo "FAIL: the pool never reported the window against its floor" >&2
    exit 1; }

stage "assert the block was recorded, and needs no distribution"
BLK_ROWS="$(sqlite3 "$POOL_DB" "SELECT COUNT(*) FROM blocks_found")"
echo "  blocks_found rows=$BLK_ROWS"
[ "$BLK_ROWS" -ge 1 ] || { echo "FAIL: the block was not recorded" >&2; exit 1; }

stage "a MIXED window: some claims paid, the smallest forfeited on chain"
# The one path everything above leaves untouched: a window holding claims of
# very different sizes, where the floor pays some and forfeits the rest, and
# the forfeit is visible in the block.
#
# It cannot be mined for. Share difficulty is clamped to network difficulty on
# regtest, so a 2.0x window holds about two shares -- which is why every stage
# above reports "window of 1 miner(s)". Two levers fix that: a much wider
# window multiple, and a share history seeded before the pool starts.
#
# Seeded, not faked. The shares table is the pool's own record of accepted
# work, and writing it while the pool is STOPPED is replaying history, not
# stubbing the thing under test. Everything downstream is real: the window
# query, the split, the coinbase builder, the block, and the outputs read back
# off the chain.
kill "$POOL_PID" 2>/dev/null || true
wait "$POOL_PID" 2>/dev/null || true
POOL_PID=""

# Three miners at 100 : 10 : 1, on a fresh ledger so the counts are exactly
# what this stage put there.
MIX_DB="/tmp/simplepool-cbmix.db"
MIX_LOG="/tmp/simplepool-cbmix.log"
MIX_CONF="/tmp/simplepool-cbmix.conf"
rm -f "$MIX_DB" "$MIX_DB-wal" "$MIX_DB-shm"
sqlite3 "$MIX_DB" < "$ROOT/schema.sql" > /dev/null

BIG="bcrt1qzyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3lgth6c"    # 100 shares
MID="bcrt1qyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zs4w3j0"    # 10
SMALL="bcrt1qxvenxvenxvenxvenxvenxvenxvenxvenztev8a"  # 1  -> under the floor

# Each seeded share carries the network difficulty a real one would, so a
# window multiple of N covers N shares.
NETDIFF="$(sqlite3 "$POOL_DB" "SELECT network_difficulty FROM pool_meta WHERE id=1")"
[ -n "$NETDIFF" ] || { echo "FAIL: no network difficulty to size the window" >&2; exit 1; }
echo "  seeding 111 shares at difficulty $NETDIFF (100 : 10 : 1)"
{
  echo "BEGIN;"
  echo "INSERT INTO workers (id,name,payout_address,first_seen,last_seen) VALUES"
  echo "  (1,'$BIG','$BIG',1,1),(2,'$MID','$MID',1,1),(3,'$SMALL','$SMALL',1,1);"
  for i in $(seq 1 100); do echo "INSERT INTO shares (worker_id,ts,difficulty) VALUES (1,1,$NETDIFF);"; done
  for i in $(seq 1 10);  do echo "INSERT INTO shares (worker_id,ts,difficulty) VALUES (2,1,$NETDIFF);"; done
  echo "INSERT INTO shares (worker_id,ts,difficulty) VALUES (3,1,$NETDIFF);"
  echo "COMMIT;"
} | sqlite3 "$MIX_DB"

# The floor sits between the 1-share claim and the 10-share one. Of a
# 4,950,000,000-sat payable amount: 100/111 = ~4.46e9, 10/111 = ~4.46e8,
# 1/111 = ~4.46e7. A floor of 100,000,000 forfeits exactly the last.
MIX_FLOOR=100000000
sed -e "s|^db_path = .*|db_path = ${MIX_DB}|" \
    -e "s|^pplns_window_diff_multiple = .*|pplns_window_diff_multiple = 200.0|" \
    -e "s|^pplns_payout_floor_sats = .*|pplns_payout_floor_sats = ${MIX_FLOOR}|" \
    "$POOL_CONF" > "$MIX_CONF"
grep -q "^pplns_payout_floor_sats" "$MIX_CONF" || \
    echo "pplns_payout_floor_sats = ${MIX_FLOOR}" >> "$MIX_CONF"
grep -q "^pplns_window_diff_multiple = 200.0" "$MIX_CONF" || {
    echo "FAIL: could not widen the window in the generated config" >&2; exit 1; }

"$POOL_BIN" "$MIX_CONF" > "$MIX_LOG" 2>&1 &
POOL_PID=$!
for _ in $(seq 1 20); do nc -z 127.0.0.1 "$POOL_PORT" 2>/dev/null && break; sleep 1; done
kill -0 "$POOL_PID" 2>/dev/null || {
    echo "FAIL: simplepool died on the mixed-window config" >&2
    tail -20 "$MIX_LOG" >&2; exit 1; }

# The pool must SAY the small miner is about to earn nothing, before a block
# makes it true. That warning is the operator's only chance to act.
#
# Up to 60s, because the FIRST job of a process carries no window -- network
# difficulty is unread until a template arrives -- and the tip watcher only
# rebuilds on a new tip or its 30-second refresh. A 20-second wait looked like
# "the pool never warned" when it simply had not built a second job yet.
for _ in $(seq 1 60); do
    grep -q "below the ${MIX_FLOOR}-sat payout floor" "$MIX_LOG" && break
    sleep 1
done
grep -q "below the ${MIX_FLOOR}-sat payout floor" "$MIX_LOG" || {
    echo "FAIL: the pool never warned that a miner falls below the floor" >&2
    grep -i "floor" "$MIX_LOG" | tail -5 >&2; exit 1; }
echo "  warned: $(grep -o '[0-9]* of [0-9]* miner(s) in the window are below' "$MIX_LOG" | tail -1)"

MIX_BEFORE=$(cli getblockcount)
node "$ROOT/scripts/regtest/cpuminer.js" --port "$POOL_PORT" --user "$MINER_ADDR" --timeout 180
MIX_AFTER=$(cli getblockcount)
[ "$MIX_AFTER" -gt "$MIX_BEFORE" ] || {
    echo "FAIL: no block was mined on the mixed window" >&2; exit 1; }

stage "assert the forfeit happened, in the block itself"
MIX_TIP="$(cli getbestblockhash)"
MIX_CB="$(cli getblock "$MIX_TIP" 2 | jq -c '.tx[0]')"
CB_JSON="$MIX_CB" BIG="$BIG" MID="$MID" SMALL="$SMALL" \
OPERATOR_ADDR="$OPERATOR_ADDR" MIX_FLOOR="$MIX_FLOOR" python3 - <<'PY'
import json, os, sys

cb    = json.loads(os.environ['CB_JSON'])
big   = os.environ['BIG']
mid   = os.environ['MID']
small = os.environ['SMALL']
op    = os.environ['OPERATOR_ADDR']
floor = int(os.environ['MIX_FLOOR'])

paid = {}
for o in cb['vout']:
    spk = o['scriptPubKey']
    if spk.get('type') == 'nulldata':
        continue
    paid[spk['address']] = paid.get(spk['address'], 0) + round(o['value'] * 1e8)

for a, v in sorted(paid.items(), key=lambda kv: -kv[1]):
    who = {big: 'BIG (100 shares)', mid: 'MID (10)', small: 'SMALL (1)',
           op: 'operator'}.get(a, 'UNKNOWN')
    print(f"    {v:>14} sats -> {a}  ({who})")

# The two claims above the floor are paid, in the block.
for name, addr in (('BIG', big), ('MID', mid)):
    if addr not in paid:
        print(f"FAIL: {name} clears the floor but has no coinbase output",
              file=sys.stderr)
        sys.exit(1)

# The one below it is NOT -- this is the whole point of the stage.
if small in paid:
    print(f"FAIL: SMALL is worth less than the {floor}-sat floor but was paid "
          f"{paid[small]} anyway", file=sys.stderr)
    sys.exit(1)

# And its money went to the operator, not nowhere. The operator must hold
# strictly more than the 1% fee, and the block must still be spent whole:
# a forfeit that vanished would show up as a coinbase paying out less than
# it may, which is value destroyed rather than merely redirected.
total = sum(paid.values())
if total != 5000000000:
    print(f"FAIL: the coinbase pays {total}, not the whole 50 BTC block",
          file=sys.stderr)
    sys.exit(1)
fee_only = 50000000
op_sats = paid.get(op, 0)
if op_sats <= fee_only:
    print(f"FAIL: operator holds {op_sats}, no more than the {fee_only}-sat "
          f"fee — the forfeited claim went nowhere", file=sys.stderr)
    sys.exit(1)
forfeited = op_sats - fee_only
print(f"  forfeited to the operator: {forfeited} sats "
      f"(on top of the {fee_only}-sat fee)")
# Roughly 1/111 of the payable amount. Bounded rather than exact because the
# block finder's own share may or may not have entered the window before the
# job was built; either way the SMALL claim is the one that lost.
if not (40000000 <= forfeited <= 50000000):
    print(f"FAIL: forfeited {forfeited} sats, expected ~44.6M "
          f"(1 share in 111 of the payable amount)", file=sys.stderr)
    sys.exit(1)
PY

# And the pool reported it, with the numbers, so an operator answering "why
# was I not paid?" has something to answer from.
grep -q "were forfeited to the operator" "$MIX_LOG" || {
    echo "FAIL: the block paid a forfeit but the pool never reported one" >&2
    grep -i "pplns-coinbase: block" "$MIX_LOG" | tail -3 >&2; exit 1; }
echo "  reported: $(grep -o '[0-9]* claim(s) worth [0-9]* sats were forfeited' "$MIX_LOG" | tail -1)"

# Still no ledger. A forfeit is income, not a debt -- if this mode ever grew a
# carry it would show up here first.
MIX_ROWS="$(sqlite3 "$MIX_DB" "SELECT COUNT(*) FROM pps_credits")"
[ "$MIX_ROWS" = "0" ] || {
    echo "FAIL: a forfeit created $MIX_ROWS ledger row(s); it must create none" >&2
    exit 1; }
echo "  pps_credits rows=0 — forfeited, not carried"

echo
echo "cbwin-e2e: PASS (the window was paid from the block's own coinbase,"
echo "                 and the pool never held the reward)"
