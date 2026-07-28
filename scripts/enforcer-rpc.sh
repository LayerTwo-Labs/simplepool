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
# Unary RPCs only. Server-streaming RPCs (e.g. SubscribeEvents) need the
# Connect envelope framing — use grpcurl or buf curl for those.
#
# Env:
#   ENFORCER_URL   base URL                        (default http://127.0.0.1:50051)
#   RPC_TIMEOUT    max seconds for the whole call  (default 60)
#
# Requires curl only.

set -euo pipefail

ENFORCER_URL="${ENFORCER_URL:-http://127.0.0.1:50051}"
RPC_TIMEOUT="${RPC_TIMEOUT:-60}"

RPC="${1:?usage: enforcer-rpc.sh <pkg.Service/Method> [json-request]}"
BODY="${2:-}"
[[ -n "$BODY" ]] || BODY='{}'

exec curl -sS --fail-with-body -m "$RPC_TIMEOUT" \
    -H 'Content-Type: application/json' \
    -H 'Connect-Protocol-Version: 1' \
    -d "$BODY" \
    "$ENFORCER_URL/$RPC"
