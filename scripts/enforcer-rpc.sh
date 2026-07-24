#!/usr/bin/env bash
# Call the bip300301_enforcer over ConnectRPC — replaces grpcurl.
#
# The enforcer serves the Connect protocol on the same port as its gRPC
# (default 127.0.0.1:50051), so a unary RPC is a plain JSON POST:
#
#   scripts/enforcer-rpc.sh cusf.mainchain.v1.ValidatorService/GetSidechains
#   scripts/enforcer-rpc.sh cusf.mainchain.v1.WalletService/CreateDepositTransaction \
#       '{"sidechain_id":9, "address":"...", "value_sats":100000000, "fee_sats":1000}'
#
# Server-streaming RPCs (e.g. GenerateBlocks) can't take a bare JSON
# POST — Connect wraps the request in a 5-byte envelope (flag byte +
# big-endian payload length) under Content-Type application/connect+json.
# --stream sends that envelope, then simply dumps the raw response
# frames until the server closes the stream (for GenerateBlocks that's
# when mining is done). No response parsing — verify the outcome with a
# follow-up unary call:
#
#   scripts/enforcer-rpc.sh --stream cusf.mainchain.v1.WalletService/GenerateBlocks \
#       '{"blocks":1}'
#
# Env:
#   ENFORCER_URL   base URL                        (default http://127.0.0.1:50051)
#   RPC_TIMEOUT    max seconds for the whole call  (default 60)
#
# Requires curl only.

set -euo pipefail

ENFORCER_URL="${ENFORCER_URL:-http://127.0.0.1:50051}"
RPC_TIMEOUT="${RPC_TIMEOUT:-60}"

STREAM=0
if [[ "${1:-}" == "--stream" ]]; then
    STREAM=1
    shift
fi
RPC="${1:?usage: enforcer-rpc.sh [--stream] <pkg.Service/Method> [json-request]}"
BODY="${2:-}"
[[ -n "$BODY" ]] || BODY='{}'

if [[ "$STREAM" == 0 ]]; then
    exec curl -sS --fail-with-body -m "$RPC_TIMEOUT" \
        -H 'Content-Type: application/json' \
        -H 'Connect-Protocol-Version: 1' \
        -d "$BODY" \
        "$ENFORCER_URL/$RPC"
fi

# 5-byte Connect envelope: 0x00 flag + big-endian uint32 payload length.
LEN=$(printf '%s' "$BODY" | wc -c)
HDR="$(printf '\\x00\\x%02x\\x%02x\\x%02x\\x%02x' \
    $(( LEN >> 24 & 255 )) $(( LEN >> 16 & 255 )) $(( LEN >> 8 & 255 )) $(( LEN & 255 )))"
{ printf "$HDR"; printf '%s' "$BODY"; } | curl -sS -m "$RPC_TIMEOUT" \
    -H 'Content-Type: application/connect+json' \
    -H 'Connect-Protocol-Version: 1' \
    --data-binary @- \
    "$ENFORCER_URL/$RPC"
echo   # raw frames aren't newline-terminated
