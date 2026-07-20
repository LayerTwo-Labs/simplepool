#!/usr/bin/env bash
# Initialise the Thunder wallet: generate a mnemonic, set the seed, and
# print a deposit address the mainchain wallet can target.
#
# Idempotent — re-running once the seed is set is a no-op that prints
# the existing deposit address.
#
# Requires: thunder + thunder-cli in .regtest/bin (via setup.sh), Thunder
# process running (via start.sh).

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$ROOT/.regtest/bin"
TCLI="$BIN/thunder-cli"

if [[ ! -x "$TCLI" ]]; then
    echo "missing $TCLI — run scripts/regtest/setup.sh" >&2
    exit 1
fi

# Probe: does Thunder's wallet already have a seed? get-new-address
# succeeds silently when yes; errors "no seed" to stderr when no.
if ADDR="$($TCLI get-new-address 2>/dev/null)" && [[ -n "$ADDR" ]]; then
    echo "==> Thunder wallet already initialised"
    echo "  new address: $ADDR"
    exit 0
fi

echo "==> generating fresh Thunder wallet mnemonic"
MNEMONIC="$($TCLI generate-mnemonic)"
echo "  mnemonic: $MNEMONIC"

echo "==> setting seed"
$TCLI set-seed-from-mnemonic "$MNEMONIC" >/dev/null

ADDR="$($TCLI get-new-address)"

echo ""
echo "Thunder wallet ready."
echo "  new address: $ADDR"
echo ""
# NOTE: pass the BARE base58 address to CreateDepositTransaction. The
# display-only 's<n>_<base58>_<hex6>' wrapper from format-deposit-address
# is NOT recognized by Thunder's OP_RETURN parser — deposits to it are
# logged as 'Ignoring invalid deposit address' and end up unpayable.
echo "To send a deposit into this wallet from the mainchain:"
echo "  grpcurl -plaintext -d '{\"sidechain_id\":9, \"address\":\"$ADDR\","
echo "    \"value_sats\":100000000, \"fee_sats\":1000}' \\"
echo "    127.0.0.1:50051 cusf.mainchain.v1.WalletService/CreateDepositTransaction"
echo "  grpcurl -plaintext -d '{\"blocks\":1}' \\"
echo "    127.0.0.1:50051 cusf.mainchain.v1.WalletService/GenerateBlocks"
