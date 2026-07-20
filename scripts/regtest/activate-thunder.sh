#!/usr/bin/env bash
# Activate sidechain #9 (Thunder) on the regtest BIP300 enforcer.
#
# Flow (driven via the enforcer's gRPC at 127.0.0.1:50051):
#   1. CreateSidechainProposal — writes an M1 message into the enforcer's
#      wallet DB. The next mined coinbase will carry it.
#   2. GenerateBlocks N --ack_all_proposals — mine + ack until the
#      proposal accumulates enough votes to activate (regtest threshold
#      is small; we mine 60 to be safe).
#   3. GetSidechains — confirm sidechain 9 is now in the active list.
#
# Requires grpcurl. Install:  brew install grpcurl
#
# Idempotent: if sidechain 9 is already active, exits 0 immediately.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GRPC="127.0.0.1:50051"
SIDECHAIN_ID=9

if ! command -v grpcurl >/dev/null 2>&1; then
    echo "grpcurl not installed; run: brew install grpcurl" >&2
    exit 1
fi

active() {
    grpcurl -plaintext "$GRPC" \
        cusf.mainchain.v1.ValidatorService/GetSidechains 2>/dev/null \
        | grep -c "\"sidechainNumber\": $SIDECHAIN_ID" || true
}

if [[ "$(active)" -gt 0 ]]; then
    echo "sidechain $SIDECHAIN_ID already active. nothing to do."
    exit 0
fi

echo "==> proposing sidechain $SIDECHAIN_ID (Thunder)"
# CreateSidechainProposal is a streaming RPC. Run it backgrounded so it
# stays alive long enough to observe the confirmation events while
# GenerateBlocks runs in the foreground.
cat > /tmp/regtest-sc-proposal.json <<EOF
{
  "sidechain_id": $SIDECHAIN_ID,
  "declaration": {
    "v0": {
      "title":       "Thunder",
      "description": "Thunder sidechain (BIP300 testbed)",
      "hash_id_1":   { "hex": "1111111111111111111111111111111111111111111111111111111111111111" },
      "hash_id_2":   { "hex": "2222222222222222222222222222222222222222" }
    }
  }
}
EOF
( grpcurl -plaintext -d @ -max-time 30 "$GRPC" \
    cusf.mainchain.v1.WalletService/CreateSidechainProposal \
    < /tmp/regtest-sc-proposal.json > /tmp/regtest-sc-proposal.out 2>&1 || true ) &
PROPOSE_PID=$!
sleep 1.5

echo "==> mining 60 blocks acking the proposal"
grpcurl -plaintext -d '{"blocks":60, "ack_all_proposals":true}' \
    -max-time 60 "$GRPC" \
    cusf.mainchain.v1.WalletService/GenerateBlocks > /tmp/regtest-mine.out 2>&1

# Streaming RPC; let it wind down then collect the last event.
sleep 1
kill "$PROPOSE_PID" 2>/dev/null || true
wait "$PROPOSE_PID" 2>/dev/null || true

if [[ "$(active)" -gt 0 ]]; then
    echo "==> sidechain $SIDECHAIN_ID is now ACTIVE"
    grpcurl -plaintext "$GRPC" cusf.mainchain.v1.ValidatorService/GetSidechains \
        | python3 -c "
import json, sys
data = json.loads(sys.stdin.read())
for s in data.get('sidechains', []):
    if s.get('sidechainNumber') == $SIDECHAIN_ID:
        print(f'  sidechainNumber={s[\"sidechainNumber\"]}')
        print(f'  proposalHeight={s.get(\"proposalHeight\")}')
        print(f'  activationHeight={s.get(\"activationHeight\")}')
        print(f'  voteCount={s.get(\"voteCount\")}')
"
    exit 0
fi

echo "!!! sidechain $SIDECHAIN_ID did NOT activate. proposal stream:" >&2
cat /tmp/regtest-sc-proposal.out >&2
exit 2
