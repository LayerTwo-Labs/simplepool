#!/usr/bin/env bash
# Activate sidechain #9 (Thunder) on the regtest BIP300 enforcer.
#
# Flow (driven via the enforcer's ConnectRPC HTTP API at 127.0.0.1:50051):
#   1. SubmitSidechainProposal — writes an M1 message into the
#      enforcer's DB. The next mined coinbase
#      will carry it.
#   2. SetAckAllProposals — persist the block-producer ack policy so every
#      coinbase the enforcer builds carries M2 acks for pending proposals.
#      The request shape changed under a fixed version string; see the call.
#   3. MiningService/GenerateToAddress — mine blocks until the
#      proposal accumulates enough votes. Walletless (enforcer PR #477);
#      requires an enforcer new enough to have the RPC. Regtest activates
#      after 6 votes; we mine in batches of 10 up to 60.
#   4. GetSidechains — confirm sidechain 9 is now in the active list.
#
# Idempotent: if sidechain 9 is already active, exits 0 immediately.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RPC="$ROOT/scripts/enforcer-rpc.sh"
export ENFORCER_URL="${ENFORCER_URL:-http://127.0.0.1:${REGTEST_ENFORCER_GRPC_PORT:-50051}}"
SIDECHAIN_ID=9
# Reward address for GenerateToAddress; any valid regtest address works.
MINE_ADDR="${REGTEST_MINE_ADDR:-bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080}"

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

echo "==> setting block-producer policy: ack all proposals"
# Upstream turned this field from a boolean into a policy enum, and did it
# WITHOUT changing the version string: v0.3.4 c188a38 takes {"ack_all": true},
# v0.3.4 9cb14bc takes {"policy": "ACK_ALL_PROPOSALS_POLICY_..."}. Its own
# migration maps the old true onto `new_slots`, so that is the faithful
# translation, not `all`.
#
# ⚠️ The LEGACY shape is tried FIRST, and the order is the whole point.
#
#   new enforcer, old field  ->  400, naming the offending field. Loud.
#   old enforcer, new field  ->  200 and the field is IGNORED. Silent.
#
# So probing with the new shape first "succeeds" against an old binary while
# setting nothing, and the failure surfaces sixty blocks later as "sidechain 9
# did NOT activate" with no hint why. Measured, not guessed: that is exactly
# what the first version of this fix did. Only the legacy-first order is
# unambiguous in both directions.
#
# The enforcer cannot be URL-pinned (see setup.sh), so a machine with a cached
# pre-change binary is a normal thing to have and should keep working.
#
# The failure path re-runs the call WITHOUT discarding stdout, because sending
# it to /dev/null is why CI reported nothing but `curl: (22)` when this broke.
# The response body names the offending field and says what is wrong.
if "$RPC" cusf.mainchain.v1.BlockProducerService/SetAckAllProposals \
        '{"ack_all": true}' >/dev/null 2>&1; then
    echo "  ack policy set (legacy ack_all=true)"
elif "$RPC" cusf.mainchain.v1.BlockProducerService/SetAckAllProposals \
        '{"policy": "ACK_ALL_PROPOSALS_POLICY_NEW_SLOTS"}' >/dev/null 2>&1; then
    echo "  ack policy set (new_slots)"
else
    echo "FAIL: SetAckAllProposals refused both the legacy boolean and the" >&2
    echo "      policy enum. The response to the current shape was:" >&2
    "$RPC" cusf.mainchain.v1.BlockProducerService/SetAckAllProposals \
        '{"policy": "ACK_ALL_PROPOSALS_POLICY_NEW_SLOTS"}' >&2 || true
    exit 1
fi

mine_batch() {
    RPC_TIMEOUT=120 "$RPC" cusf.mainchain.v1.MiningService/GenerateToAddress \
        '{"blocks": '"$1"', "address": "'"$MINE_ADDR"'"}'
}

echo "==> mining blocks until the proposal activates"
for batch in 1 2 3 4 5 6; do
    if ! mine_batch 10 > /tmp/regtest-mine.out 2>&1; then
        echo "!!! mining failed:" >&2
        cat /tmp/regtest-mine.out >&2
        if grep -q unimplemented /tmp/regtest-mine.out; then
            echo "" >&2
            echo "This enforcer predates MiningService/GenerateToAddress" >&2
            echo "(bip300301_enforcer PR #477). Upgrade the prebuilt:" >&2
            echo "  rm .regtest/bin/enforcer.zip .regtest/bin/bip300301_enforcer" >&2
            echo "  scripts/regtest/setup.sh" >&2
        fi
        exit 2
    fi
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
    echo "  not active yet after $((batch * 10)) blocks"
done

echo "!!! sidechain $SIDECHAIN_ID did NOT activate. last mine output:" >&2
cat /tmp/regtest-mine.out >&2
exit 2
