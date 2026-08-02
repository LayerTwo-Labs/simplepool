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
BIN="${REGTEST_BIN_DIR:-${REGTEST_DIR:-$ROOT/.regtest}/bin}"
THUNDER_RPC_PORT="${REGTEST_THUNDER_RPC_PORT:-6009}"
TCLI="$BIN/thunder-cli"
tcli() { "$TCLI" --rpc-url="http://127.0.0.1:$THUNDER_RPC_PORT" "$@"; }

if [[ ! -x "$TCLI" ]]; then
    echo "missing $TCLI — run scripts/regtest/setup.sh" >&2
    exit 1
fi

# Probe: does Thunder's wallet already have a seed? get-new-address
# succeeds silently when yes; errors "no seed" to stderr when no.
if ADDR="$(tcli get-new-address 2>/dev/null)" && [[ -n "$ADDR" ]]; then
    echo "==> Thunder wallet already initialised"
    echo "  new address: $ADDR"
    exit 0
fi

echo "==> generating fresh Thunder wallet mnemonic"
MNEMONIC="$(tcli generate-mnemonic)"
echo "  mnemonic: $MNEMONIC"

echo "==> setting seed"
tcli set-seed-from-mnemonic "$MNEMONIC" >/dev/null

ADDR="$(tcli get-new-address)"

echo ""
echo "Thunder wallet ready."
echo "  new address: $ADDR"
echo ""
# NOTE: pass the BARE base58 address to CreateDepositTransaction. The
# display-only 's<n>_<base58>_<hex6>' wrapper from format-deposit-address
# is NOT recognized by Thunder's OP_RETURN parser — deposits to it are
# logged as 'Ignoring invalid deposit address' and end up unpayable.
echo "To send a deposit into this wallet from the mainchain:"
echo "  scripts/enforcer-rpc.sh cusf.mainchain.v1.WalletService/CreateDepositTransaction \\"
echo "    '{\"sidechain_id\":9, \"address\":\"$ADDR\", \"value_sats\":100000000, \"fee_sats\":1000}'"
echo "  # mine it:"
echo "  scripts/enforcer-rpc.sh cusf.mainchain.v1.MiningService/GenerateToAddress \\"
echo "    '{\"blocks\":1, \"address\":\"bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080\"}'"
