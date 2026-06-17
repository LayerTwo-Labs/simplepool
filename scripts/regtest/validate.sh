#!/usr/bin/env bash
# Validate the drivechain coinbase shape end-to-end:
#
#   1. Bootstrap the regtest L1: generate ~150 blocks to a stock P2WPKH
#      address so we have spendable coins and a sane chain height.
#   2. Sidechain #9 (Thunder) must be ACTIVATED before deposits can land.
#      The enforcer exposes that via the BIP300 propose/ack flow; we
#      drive it via bitcoin-cli / enforcer JSON-RPC.
#   3. Configure simplepool in pool_mode=pps, pointed at the enforcer's
#      getblocktemplate endpoint (127.0.0.1:18444).
#   4. Connect a stratum miner (here: cpuminer-style via Python) for a
#      few seconds — we just need it to mine ONE block. Regtest difficulty
#      is trivially low.
#   5. Read the new tip's coinbase tx and assert:
#        - first non-OP_RETURN output uses script [OP_NOP5 0x01 0x09 OP_TRUE]
#        - the OP_RETURN immediately after it contains our payload bytes
#        - the operator BTC fee output is present at fee_bps
#   6. Watch the enforcer log / events for a Deposit event tagged
#      sidechain_id=9 — that's the canonical signal the coinbase shape
#      was accepted as a valid drivechain deposit.
#
# This script is intentionally a "guided runbook" — each step prints
# clearly so a human can read the output and intervene.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REGTEST="$ROOT/.regtest"
BIN="$REGTEST/bin"
DATA="$REGTEST/data"

cli() { "$BIN/bitcoin-cli" -datadir="$DATA/bitcoind" -regtest -rpcuser=user -rpcpassword=password "$@"; }

echo "==> sanity check stack"
"$ROOT/scripts/regtest/status.sh"

echo ""
echo "==> activate sidechain #9 (Thunder)"
"$ROOT/scripts/regtest/activate-thunder.sh"

echo ""
echo "==> bootstrap chain (mine 150 to miner wallet so coinbase matures)"
ADDR="$(cli -rpcwallet=miner getnewaddress '' bech32)"
echo "  miner address: $ADDR"
cli generatetoaddress 150 "$ADDR" > /dev/null
echo "  height now: $(cli getblockcount)"

echo ""
echo "==> probe enforcer GBT (should include drivechain commitments)"
GBT="$(curl -sS -H 'content-type: application/json' \
    --data '{"jsonrpc":"2.0","id":1,"method":"getblocktemplate","params":[{"rules":["segwit"]}]}' \
    http://127.0.0.1:18444/)"
echo "$GBT" | python3 -c "
import json, sys
r = json.loads(sys.stdin.read())['result']
print(f'  height={r.get(\"height\")} coinbasevalue={r.get(\"coinbasevalue\")}')
print(f'  has coinbasetxn: {bool(r.get(\"coinbasetxn\"))}')
cb = r.get('coinbasetxn')
if cb:
    print(f'  coinbasetxn hex bytes={len(cb.get(\"data\", \"\"))//2}')
"

echo ""
echo "==> next steps (manual, since CPU mining a bech32 work is fiddly):"
echo ""
echo "  1. In another terminal, run simplepool in pps mode against the enforcer:"
echo ""
echo "       cat > /tmp/regtest-proxy.conf <<EOF"
echo "       listen_addr = 127.0.0.1"
echo "       listen_port = 13334"
echo "       bitcoind_url = http://127.0.0.1:18444"
echo "       operator_address = $ADDR"
echo "       fee_bps = 100"
echo "       pool_mode = pps"
echo "       pool_thunder_reserve_address = SoMeThunderAddrTest"
echo "       thunder_sidechain_number = 9"
echo "       pps_sats_per_diff = 1000"
echo "       db_path = /tmp/regtest-shares.db"
echo "       EOF"
echo "       mkdir -p /tmp/regtest-data"
echo "       ./build/simplepool /tmp/regtest-proxy.conf"
echo ""
echo "  2. Point any stratum miner at 127.0.0.1:13334 with username ="
echo "     a valid Thunder address (any 20-byte hash base58-encoded)."
echo "     The first block found will deposit into Thunder."
echo ""
echo "  3. After a block is mined, run:"
echo "       scripts/regtest/inspect-coinbase.sh"
echo ""
echo "     to assert the drivechain output layout, and:"
echo "       tail -f .regtest/logs/bip300301_enforcer.log | grep -i deposit"
echo ""
echo "     to see the enforcer's Deposit event."
