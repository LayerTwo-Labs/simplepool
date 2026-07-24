#!/usr/bin/env bash
# Activate sidechain #9 (Thunder) on the regtest BIP300 enforcer.
#
# Flow (driven via the enforcer's ConnectRPC HTTP API at 127.0.0.1:50051):
#   1. SubmitSidechainProposal — writes an M1 message into the
#      enforcer's DB. The next mined coinbase
#      will carry it.
#   2. GenerateBlocks N --ack_all_proposals — mine + ack until the
#      proposal accumulates enough votes to activate (regtest threshold
#      is small; we mine 60 to be safe).
#   3. GetSidechains — confirm sidechain 9 is now in the active list.
#
# Idempotent: if sidechain 9 is already active, exits 0 immediately.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RPC="$ROOT/scripts/enforcer-rpc.sh"
export ENFORCER_URL="${ENFORCER_URL:-http://127.0.0.1:50051}"
SIDECHAIN_ID=9

for dep in curl jq; do
    if ! command -v "$dep" >/dev/null 2>&1; then
        echo "$dep not installed" >&2
        exit 1
    fi
done

active() {
    "$RPC" cusf.mainchain.v1.ValidatorService/GetSidechains 2>/dev/null \
        | jq --argjson id "$SIDECHAIN_ID" \
            '[.sidechains[]? | select(.sidechainNumber == $id)] | length' \
            2>/dev/null \
        || echo 0
}

if [[ "$(active)" -gt 0 ]]; then
    echo "sidechain $SIDECHAIN_ID already active. nothing to do."
    exit 0
fi

echo "==> proposing sidechain $SIDECHAIN_ID (Thunder)"
PROPOSAL='{
  "sidechain_id": '$SIDECHAIN_ID',
  "declaration": {
    "v0": {
      "title":       "Thunder",
      "description": "Thunder sidechain (BIP300 testbed)",
      "hash_id_1":   { "hex": "1111111111111111111111111111111111111111111111111111111111111111" },
      "hash_id_2":   { "hex": "2222222222222222222222222222222222222222" }
    }
  }
}'
# Unary; {} on success. Re-running while a previous attempt's proposal
# is still pending gets already_exists back — fine, that pending
# proposal is exactly what the mining below acks.
if OUT="$("$RPC" cusf.mainchain.v1.BlockProducerService/SubmitSidechainProposal "$PROPOSAL" 2>&1)"; then
    echo "  proposal submitted"
elif [[ "$OUT" == *already_exists* ]]; then
    echo "  proposal already pending"
else
    echo "$OUT" >&2
    exit 1
fi

echo "==> mining 60 blocks acking the proposal"
RPC_TIMEOUT=60 "$RPC" --stream \
    cusf.mainchain.v1.WalletService/GenerateBlocks \
    '{"blocks":60, "ack_all_proposals":true}' > /tmp/regtest-mine.out 2>&1

# Give the enforcer a beat to finish processing the last block.
sleep 1

if [[ "$(active)" -gt 0 ]]; then
    echo "==> sidechain $SIDECHAIN_ID is now ACTIVE"
    "$RPC" cusf.mainchain.v1.ValidatorService/GetSidechains \
        | jq -r --argjson id "$SIDECHAIN_ID" '
            .sidechains[]? | select(.sidechainNumber == $id) |
            "  sidechainNumber=\(.sidechainNumber)",
            "  proposalHeight=\(.proposalHeight)",
            "  activationHeight=\(.activationHeight)",
            "  voteCount=\(.voteCount)"'
    exit 0
fi

echo "!!! sidechain $SIDECHAIN_ID did NOT activate. mine output:" >&2
cat /tmp/regtest-mine.out >&2
exit 2
