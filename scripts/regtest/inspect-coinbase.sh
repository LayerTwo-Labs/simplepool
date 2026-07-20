#!/usr/bin/env bash
# Read the current tip's coinbase from bitcoind and parse its outputs,
# asserting the drivechain-deposit shape the pool is supposed to emit
# in pool_mode=pps. Run this after simplepool mines a block.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REGTEST="$ROOT/.regtest"
BIN="$REGTEST/bin"
DATA="$REGTEST/data"

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
print(f'  output count: {len(outs)}')
for i, o in enumerate(outs):
    spk = o['scriptPubKey']
    asm = spk.get('asm', '')
    hex_ = spk.get('hex', '')
    val = o['value']
    print(f'  [{i}] value={val} BTC type={spk.get("type")} asm={asm[:80]} hex={hex_[:80]}')

# Look for OP_NOP5 (0xb4) + push1 + sidechain + OP_TRUE = pattern b4 01 09 51
dc_idx = None
for i, o in enumerate(outs):
    h = o['scriptPubKey']['hex'].lower()
    if h.startswith('b401') and len(h) == 8 and h.endswith('51'):
        dc_idx = i
        side = int(h[4:6], 16)
        print(f'\n  >>> OP_DRIVECHAIN found at output [{i}], sidechain={side}')

if dc_idx is None:
    print('\n  !!! NO OP_DRIVECHAIN OUTPUT FOUND — pool did not emit a drivechain coinbase')
    sys.exit(2)

# Verify next output is OP_RETURN.
nxt = outs[dc_idx + 1]
nxt_hex = nxt['scriptPubKey']['hex'].lower()
if not nxt_hex.startswith('6a'):
    print(f'  !!! output [{dc_idx + 1}] is not OP_RETURN; enforcer will reject')
    sys.exit(3)
print(f'  >>> OP_RETURN payload immediately follows; hex={nxt_hex}')
PY

echo ""
echo "==> recent enforcer log lines mentioning deposit/sidechain:"
grep -iE 'deposit|sidechain|m5' "$REGTEST/logs/bip300301_enforcer.log" | tail -20 || echo "  (none yet)"
