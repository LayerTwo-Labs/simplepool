#!/usr/bin/env bash
# Read the current tip's coinbase from bitcoind and parse its outputs,
# asserting the pps-classic shape the pool is supposed to emit: the
# net-of-fee reward paid to the pool's BTC wallet, the fee paid to the
# operator, and NO OP_DRIVECHAIN output (pool_mode=pps was removed —
# the enforcer never credited coinbase-source deposits anyway).
# Run this after simplepool mines a block.
#
# Env:
#   POOL_BTC_ADDRESS   expected coinbase spendable output (required)
#   OPERATOR_ADDRESS   expected fee output (optional; checked when set)

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REGTEST="${REGTEST_DIR:-$ROOT/.regtest}"
BIN="${REGTEST_BIN_DIR:-$REGTEST/bin}"
DATA="$REGTEST/data"

if [ -z "${POOL_BTC_ADDRESS:-}" ]; then
    echo "POOL_BTC_ADDRESS must be set to the pool's coinbase address" >&2
    exit 1
fi

cli() { "$BIN/bitcoin-cli" -datadir="$DATA/bitcoind" -regtest -rpcuser=user -rpcpassword=password "$@"; }

TIP="$(cli getbestblockhash)"
BLOCK="$(cli getblock "$TIP" 2)"
CB="$(echo "$BLOCK" | python3 -c "
import json, sys
b = json.loads(sys.stdin.read())
cb = b['tx'][0]
print(json.dumps(cb))
")"

echo "==> tip: $TIP"
echo "==> coinbase outputs:"
# Pass CB via env var because a heredoc would steal stdin from the pipe.
CB="$CB" python3 - <<'PY'
import json, os, sys

cb = json.loads(os.environ['CB'])
outs = cb['vout']
pool_addr = os.environ['POOL_BTC_ADDRESS']
op_addr = os.environ.get('OPERATOR_ADDRESS') or None

print(f'  output count: {len(outs)}')
for i, o in enumerate(outs):
    spk = o['scriptPubKey']
    asm = spk.get('asm', '')
    hex_ = spk.get('hex', '')
    print(f'  [{i}] value={o["value"]} BTC type={spk.get("type")} '
          f'addr={spk.get("address")} asm={asm[:60]} hex={hex_[:60]}')

def addrs(o):
    """bitcoind emits `address` (v22+) or a legacy `addresses` list."""
    spk = o['scriptPubKey']
    return [spk['address']] if 'address' in spk else spk.get('addresses', [])

fail = False

pool_outs = [o for o in outs if pool_addr in addrs(o)]
if not pool_outs:
    print(f'\n  !!! no coinbase output pays the pool wallet {pool_addr}')
    fail = True
else:
    print(f'\n  >>> pool output found: {pool_outs[0]["value"]} BTC -> {pool_addr}')

if op_addr:
    op_outs = [o for o in outs if op_addr in addrs(o)]
    if not op_outs:
        print(f'  !!! no coinbase output pays the operator {op_addr}')
        fail = True
    else:
        print(f'  >>> operator fee output found: {op_outs[0]["value"]} BTC -> {op_addr}')
    # The fee must be the smaller of the two — a swapped split would
    # silently pay the operator the whole block reward.
    if op_outs and pool_outs and op_outs[0]['value'] >= pool_outs[0]['value']:
        print('  !!! operator fee output is not smaller than the pool output')
        fail = True

# Regression guard: OP_DRIVECHAIN is 0xb4 <push1> <sidechain> OP_TRUE.
for i, o in enumerate(outs):
    h = o['scriptPubKey']['hex'].lower()
    if h.startswith('b401') and len(h) == 8 and h.endswith('51'):
        print(f'  !!! OP_DRIVECHAIN output at [{i}] — pool_mode=pps was removed '
              'and must not be emitted')
        fail = True

sys.exit(4 if fail else 0)
PY

echo ""
echo "==> classic coinbase shape OK"
