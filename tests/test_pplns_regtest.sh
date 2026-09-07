#!/usr/bin/env bash
# End-to-end test of the PPLNS distribution path, for BOTH rails.
#
#   bitcoind-patched  <-ZMQ/RPC-  bip300301_enforcer (walletless)
#          ^                              | GBT
#          | submitblock                  v
#          +----------------------- simplepool (pplns-thunder | pplns-btc)
#                                         ^ stratum
#                                         |
#                                  cpuminer.js
#
# What this covers that the unit tests cannot.
#
# tests/test_store.c proves store_pplns_distribute splits a window
# correctly, given a database somebody hand-built to have a matured block
# in it. Nothing proved that a real pool ever reaches that state: that a
# block found through stratum gets a window snapshotted onto its row, that
# the confirmation pass keeps counting its depth until it reaches 100, and
# that the distributor then actually runs and credits somebody. Every one
# of those is a different file, and the seam between them is where a mode
# that passes its unit tests still pays nobody.
#
# Both rails run, because pool_mode decides two things at once and only one
# of them is the rail. pplns-thunder and pplns-btc pool the reward
# identically and share every line of the distribution path; what differs
# is what a stratum username IS -- a Thunder address on one, a Bitcoin
# address on the other. So each rail authorizes with its own username shape
# against its own pool, and both must reach the same credited ledger. A
# regression that broke username validation for one rail would otherwise
# hide behind the other passing.
#
# The maturity gate is asserted as an absence before it is asserted as a
# presence. A confirmed-but-shallow block must credit NOBODY: crediting a
# coinbase before it is spendable creates a balance the pool cannot fund,
# which is the reserve requirement PPLNS exists to remove. Asserting only
# the end state would pass just as well against a distributor with no
# maturity check at all, which is the exact bug worth catching.
#
# Deterministic by construction: a fresh chain and a fresh database every
# run, its own data dir, and per-run ports, so it coexists with a dev stack
# in .regtest/ and with the other two e2e suites.
#
# Env:
#   REGTEST_DIR      data dir, WIPED each run (default: <repo>/.regtest-pplns)
#   REGTEST_BIN_DIR  binary cache, kept across runs (default: <repo>/.regtest/bin)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
export REGTEST_DIR="${REGTEST_DIR:-$ROOT/.regtest-pplns}"
export REGTEST_BIN_DIR="${REGTEST_BIN_DIR:-$ROOT/.regtest/bin}"
export REGTEST_SKIP_THUNDER=1
export REGTEST_WALLETLESS=1

BIN="$REGTEST_BIN_DIR"
POOL_BIN="$ROOT/build/simplepool"
RPC="$ROOT/scripts/enforcer-rpc.sh"

# Maturity, and the depth the proxy itself uses (PPLNS_MATURITY_CONFS and
# BLOCK_FINAL_DEPTH in src/main.c). Kept as a name so the two places that
# mine toward it cannot drift apart.
MATURITY=100
# Deliberately short of maturity: deep enough that the block is certainly
# confirmed, shallow enough that crediting it would be a bug.
SHALLOW=10

OPERATOR_ADDR="bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"
# Distinct from OPERATOR_ADDR so the fee output and the pool output are
# distinguishable on-chain.
POOL_BTC_ADDR="bcrt1qqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5phstwt"
# What a miner types as its username, per rail. The Bitcoin one is a
# different address again from the two above: it is a payout destination,
# not a coinbase output, and conflating them is how a test passes while
# paying the wrong party.
BTC_USER="bcrt1qzyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3lgth6c"
THUNDER_USER="11111111111111111111"
FEE_BPS=100

POOL_PID=""

cli() { "$BIN/bitcoin-cli" -datadir="$REGTEST_DIR/data/bitcoind" -regtest \
        -rpcuser=user -rpcpassword=password "$@"; }

# Mine through the enforcer rather than bitcoind directly, so the enforcer's
# own view of the chain advances with it -- it is the thing serving GBT, and
# a tip it has not seen produces no template and no confirmation pass.
mine() { RPC_TIMEOUT=180 "$RPC" cusf.mainchain.v1.MiningService/GenerateToAddress \
         '{"blocks": '"$1"', "address": "'"$OPERATOR_ADDR"'"}' >/dev/null; }

q() { sqlite3 "$POOL_DB" "$1"; }

stage() { echo; echo "=== pplns-e2e: $1"; }

dump_logs() {
    echo "!!! pplns e2e FAILED — recent logs:" >&2
    for f in "$REGTEST_DIR"/logs/*.log "${POOL_LOG:-}"; do
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
    echo "If it crashed and left the lock behind, clear it with:" >&2
    echo "  REGTEST_DIR=$REGTEST_DIR scripts/regtest/stop.sh && rm -rf $LOCK" >&2
    exit 1
fi
trap 'code=$?; [ "$code" -ne 0 ] && dump_logs; cleanup; exit $code' EXIT
trap 'exit 130' INT TERM

for dep in sqlite3 jq node nc curl; do
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
echo "  bitcoind=$REGTEST_BITCOIND_RPC_PORT enforcer=$REGTEST_ENFORCER_RPC_PORT/$REGTEST_ENFORCER_GRPC_PORT pool=$POOL_PORT"

stage "wipe data dir (fresh chain every run)"
rm -rf "$REGTEST_DIR/data" "$REGTEST_DIR/logs" "$REGTEST_DIR/run"

stage "build simplepool"
make -C "$ROOT" -j >/dev/null

stage "download prebuilt binaries"
"$ROOT/scripts/regtest/setup.sh"

stage "start bitcoind-patched + walletless enforcer"
"$ROOT/scripts/regtest/start.sh"

stage "activate sidechain #9 via enforcer-template mining"
# PPLNS emits a classic coinbase, same as pps-classic, so the sidechain is
# not what is under test. It is activated anyway so the enforcer's template
# still carries the BIP301 commitment outputs the coinbase builder has to
# preserve -- mining against a template without them would exercise an
# easier case than production ever runs.
"$ROOT/scripts/regtest/activate-thunder.sh"

# ---------------------------------------------------------------------------
# One rail.
# ---------------------------------------------------------------------------
run_rail() {
    local mode="$1" user="$2"
    POOL_CONF="/tmp/simplepool-pplns-$mode.conf"
    POOL_LOG="/tmp/simplepool-pplns-$mode.log"
    POOL_DB="/tmp/simplepool-pplns-$mode.db"

    stage "[$mode] start simplepool against enforcer GBT"
    rm -f "$POOL_DB" "$POOL_DB-wal" "$POOL_DB-shm"
    cat > "$POOL_CONF" <<EOF
listen_addr = 127.0.0.1
listen_port = ${POOL_PORT}

bitcoind_url = http://127.0.0.1:${REGTEST_ENFORCER_RPC_PORT}
bitcoind_poll_interval_ms = 500

operator_address = ${OPERATOR_ADDR}
fee_bps = ${FEE_BPS}
coinbase_tag = /simplepool-pplns/

pool_mode = ${mode}
pool_btc_address = ${POOL_BTC_ADDR}
# The window is a multiple of network difficulty, which on regtest is
# ~4.66e-10. Any positive multiple is filled by the handful of shares
# cpuminer.js produces before it finds a block, so the whole reward is
# distributed and the arithmetic below stays exact.
pplns_window_diff_multiple = 2.0

# Clamped down to the network difficulty at connect time, so any nonce that
# finds a block also passes the share check (see cpuminer.js).
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

    stage "[$mode] mine one block through stratum as $user"
    local before after
    before=$(cli getblockcount)
    node "$ROOT/scripts/regtest/cpuminer.js" --port "$POOL_PORT" --user "$user" --timeout 180
    after=$(cli getblockcount)
    echo "  height: $before -> $after"
    [ "$after" -gt "$before" ] || {
        echo "FAIL: [$mode] block submitted but the chain did not advance" >&2; exit 1; }

    # The username rule is half of what pool_mode decides, so prove the pool
    # actually took this rail's shape rather than accepting anything.
    local nworkers
    nworkers=$(q "SELECT COUNT(*) FROM workers WHERE name = '$user'")
    [ "$nworkers" = "1" ] || {
        echo "FAIL: [$mode] '$user' was not authorized as a worker" >&2; exit 1; }

    stage "[$mode] the block carries a snapshotted window"
    local hash window
    hash=$(q "SELECT hash FROM blocks_found ORDER BY id DESC LIMIT 1")
    [ -n "$hash" ] || { echo "FAIL: [$mode] no block row was written" >&2; exit 1; }
    window=$(q "SELECT pplns_window_diff FROM blocks_found WHERE hash='$hash'")
    echo "  block $hash window=$window"
    # Snapshotted at find time; a zero window is skipped by the distributor,
    # so this is the difference between paying out and silently never paying.
    awk -v w="$window" 'BEGIN { exit !(w > 0) }' || {
        echo "FAIL: [$mode] block was recorded with no PPLNS window" >&2; exit 1; }

    stage "[$mode] a confirmed but immature block credits nobody"
    mine "$SHALLOW"
    # The proxy reconciles only when the tip HEIGHT changes, and it reaches
    # the next tip through a long poll that can sit for 30s. So drive it:
    # mine one more block per attempt rather than waiting on a cadence this
    # test does not control. Each nudge is one confirmation deeper, and the
    # budget here stays far short of $MATURITY -- the assertion below checks
    # that rather than assuming it.
    local confs credited
    for _ in $(seq 1 40); do
        confs=$(q "SELECT COALESCE(confirmations,0) FROM blocks_found WHERE hash='$hash'")
        [ "${confs:-0}" -ge 1 ] && break
        mine 1
        sleep 3
    done
    echo "  confirmations=$confs (maturity is $MATURITY)"
    [ "${confs:-0}" -ge 1 ] || {
        echo "FAIL: [$mode] block never reached even one confirmation" >&2; exit 1; }
    # The whole point of this stage: it must still be short of maturity, or
    # it proves nothing about the gate.
    [ "${confs:-0}" -lt "$MATURITY" ] || {
        echo "FAIL: [$mode] block reached $confs confirmations before the" >&2
        echo "      immaturity check could run — the nudge budget above is" >&2
        echo "      too large relative to maturity ($MATURITY)" >&2
        exit 1; }
    credited=$(q "SELECT COALESCE(SUM(accrued_sats),0) FROM pps_credits")
    [ "$credited" = "0" ] || {
        echo "FAIL: [$mode] $credited sats credited from a block only $confs deep —" >&2
        echo "      a coinbase is unspendable until $MATURITY, so this is a balance" >&2
        echo "      the pool cannot fund" >&2
        exit 1; }
    echo "  credited nothing, as required"

    stage "[$mode] mature the block and distribute"
    mine "$MATURITY"
    local dist
    for _ in $(seq 1 40); do
        dist=$(q "SELECT COALESCE(pplns_distributed,0) FROM blocks_found WHERE hash='$hash'")
        [ "${dist:-0}" = "1" ] && break
        mine 1          # same nudge: a tip change is what runs the pass
        sleep 3
    done
    confs=$(q "SELECT COALESCE(confirmations,0) FROM blocks_found WHERE hash='$hash'")
    echo "  confirmations=$confs distributed=$dist"
    [ "${dist:-0}" = "1" ] || {
        echo "FAIL: [$mode] block is $confs deep and still undistributed" >&2
        echo "      (maturity $MATURITY) — the distributor never ran or never" >&2
        echo "      considered it eligible" >&2
        exit 1; }

    stage "[$mode] the credited ledger matches the block, net of the fee"
    local reward fee gross payable total
    reward=$(q "SELECT COALESCE(reward_sats,0) FROM blocks_found WHERE hash='$hash'")
    fee=$(q "SELECT COALESCE(fee_sats,0)    FROM blocks_found WHERE hash='$hash'")
    gross=$(( reward + fee ))
    # Same truncating arithmetic as store_pplns_distribute.
    payable=$(( gross - (gross * FEE_BPS) / 10000 ))
    total=$(q "SELECT COALESCE(SUM(accrued_sats),0) FROM pps_credits")
    echo "  reward=$reward fee=$fee gross=$gross payable=$payable credited=$total"
    # Fees are included deliberately: PPLNS shares what the block actually
    # earned, not a subsidy-only estimate.
    [ "$total" = "$payable" ] || {
        echo "FAIL: [$mode] credited $total sats, expected $payable" >&2; exit 1; }
    # One miner, so the whole payable amount is its own -- and it must be
    # THIS rail's username that holds it.
    local mine_sats
    mine_sats=$(q "SELECT COALESCE(SUM(c.accrued_sats),0) FROM pps_credits c
                     JOIN workers w ON w.id = c.worker_id WHERE w.name = '$user'")
    [ "$mine_sats" = "$payable" ] || {
        echo "FAIL: [$mode] '$user' holds $mine_sats of $payable" >&2; exit 1; }

    stage "[$mode] distribution is exactly once"
    # Crediting is additive and there is no negative share, so a second pass
    # over the same block doubles every balance and leaves no trace in the
    # amounts themselves. Give the confirmation pass several more tips to
    # run over an already-distributed block.
    mine 5
    sleep 8
    local again
    again=$(q "SELECT COALESCE(SUM(accrued_sats),0) FROM pps_credits")
    [ "$again" = "$payable" ] || {
        echo "FAIL: [$mode] balance moved from $payable to $again after further" >&2
        echo "      passes — the block was distributed more than once" >&2
        exit 1; }
    echo "  balance still $again after 5 more tips"

    kill "$POOL_PID" 2>/dev/null || true
    wait "$POOL_PID" 2>/dev/null || true
    POOL_PID=""
}

run_rail pplns-thunder "$THUNDER_USER"
run_rail pplns-btc     "$BTC_USER"

echo
echo "pplns e2e: PASS (both rails distributed a matured block, exactly once)"
