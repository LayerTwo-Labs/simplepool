#!/usr/bin/env bash
# Spin up a local BIP300 regtest stack to validate the pool's drivechain
# coinbase shape end-to-end.
#
# Stack:
#   bitcoind-patched   — BIP300/301-aware Bitcoin Core fork (LayerTwo-Labs)
#   electrs            — Electrum server the enforcer indexes from
#   bip300301_enforcer — validator that watches the BTC chain for deposits
#
# Thunder itself isn't started here — there's no aarch64-darwin prebuilt
# and the enforcer is the authoritative deposit validator. Adding Thunder
# is a follow-up if you want to see the credit show up on the sidechain
# wallet UI.
#
# State lives under .regtest/ (gitignored).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REGTEST="$ROOT/.regtest"
BIN="$REGTEST/bin"
DATA="$REGTEST/data"
LOGS="$REGTEST/logs"
mkdir -p "$BIN" "$DATA/bitcoind" "$DATA/electrs" "$DATA/enforcer" "$DATA/thunder" "$LOGS"

# ---- arch detection ----
UNAME_M="$(uname -m)"
UNAME_S="$(uname -s)"
case "$UNAME_S/$UNAME_M" in
    Darwin/arm64) ARCH=aarch64-apple-darwin ;;
    Darwin/x86_64) ARCH=x86_64-apple-darwin ;;
    Linux/x86_64) ARCH=x86_64-unknown-linux-gnu ;;
    *) echo "unsupported platform $UNAME_S/$UNAME_M" >&2; exit 1 ;;
esac

# ---- helpers ----
fetch_zip() {
    local url="$1"
    local out="$2"
    if [[ -f "$out" ]]; then
        echo "  already have $(basename "$out")"
        return
    fi
    echo "  downloading $(basename "$out")"
    curl -fsSL -o "$out.tmp" "$url"
    mv "$out.tmp" "$out"
}

extract_to_bin() {
    local zip="$1"
    local match="$2"    # glob (e.g. '*/bitcoind', '*bip300301-enforcer*')
    local final="$3"    # canonical name we want in $BIN/
    if [[ -x "$BIN/$final" ]]; then
        echo "  $final already extracted"
        return
    fi
    # -j strips directories. Some zips have only one file (electrs).
    unzip -qq -j -o "$zip" "$match" -d "$BIN"
    # Find the most-recently-extracted file and rename to $final if needed.
    # Exclude already-canonical filenames so each extraction is idempotent
    # regardless of what got extracted before it.
    local actual
    actual="$(ls -t "$BIN" | grep -v -E '\.zip$|^(bitcoind|bitcoin-cli|electrs|bip300301_enforcer|thunder|thunder-cli)$' | head -1 || true)"
    if [[ -n "$actual" && "$actual" != "$final" ]]; then
        mv "$BIN/$actual" "$BIN/$final"
    fi
    chmod +x "$BIN/$final"
}

# ---- download prebuilts ----
echo "==> fetching prebuilt binaries ($ARCH)"
case "$ARCH" in
    aarch64-apple-darwin)
        BITCOIN_ZIP_URL="https://releases.drivechain.info/L1-bitcoin-patched-v30.2-aarch64-apple-darwin.zip"
        ENFORCER_ZIP_URL="https://releases.drivechain.info/bip300301-enforcer-latest-aarch64-apple-darwin.zip"
        ELECTRS_ZIP_URL="https://releases.drivechain.info/electrs-latest-aarch64-apple-darwin.zip"
        THUNDER_ZIP_URL="https://releases.drivechain.info/L2-S9-Thunder-latest-aarch64-apple-darwin.zip"
        ;;
    x86_64-apple-darwin)
        BITCOIN_ZIP_URL="https://releases.drivechain.info/L1-bitcoin-patched-latest-x86_64-apple-darwin.zip"
        ENFORCER_ZIP_URL="https://releases.drivechain.info/bip300301-enforcer-latest-x86_64-apple-darwin.zip"
        ELECTRS_ZIP_URL="https://releases.drivechain.info/electrs-latest-x86_64-apple-darwin.zip"
        THUNDER_ZIP_URL="https://releases.drivechain.info/L2-S9-Thunder-latest-x86_64-apple-darwin.zip"
        ;;
    *)
        echo "no prebuilt binaries for $ARCH — build from source" >&2
        exit 1
        ;;
esac

fetch_zip "$BITCOIN_ZIP_URL"  "$BIN/bitcoind.zip"
fetch_zip "$ENFORCER_ZIP_URL" "$BIN/enforcer.zip"
fetch_zip "$ELECTRS_ZIP_URL"  "$BIN/electrs.zip"
fetch_zip "$THUNDER_ZIP_URL"  "$BIN/thunder.zip"

echo "==> extracting binaries"
extract_to_bin "$BIN/bitcoind.zip"  '*/bitcoind'                 bitcoind
extract_to_bin "$BIN/bitcoind.zip"  '*/bitcoin-cli'              bitcoin-cli
extract_to_bin "$BIN/enforcer.zip"  '*bip300301-enforcer*'       bip300301_enforcer
extract_to_bin "$BIN/electrs.zip"   '*electrs*'                  electrs
extract_to_bin "$BIN/thunder.zip"   'thunder-latest-*'           thunder
extract_to_bin "$BIN/thunder.zip"   'thunder-cli-latest-*'       thunder-cli

echo "==> binaries ready in $BIN"
ls -la "$BIN" | tail -n +2

# ---- write configs ----
echo "==> writing configs"

cat > "$DATA/bitcoind/bitcoin.conf" <<EOF
regtest=1
server=1
listen=1
txindex=1
rest=1
fallbackfee=0.0001
[regtest]
rpcuser=user
rpcpassword=password
rpcport=18443
zmqpubrawblock=tcp://127.0.0.1:29000
zmqpubsequence=tcp://127.0.0.1:29000
EOF

cat > "$DATA/electrs/config.toml" <<EOF
network = "regtest"
db_dir = "$DATA/electrs/db"
daemon_dir = "$DATA/bitcoind/regtest"
daemon_rpc_addr = "127.0.0.1:18443"
daemon_p2p_addr = "127.0.0.1:18444"
auth = "user:password"
electrum_rpc_addr = "127.0.0.1:60401"
monitoring_addr = "127.0.0.1:24225"
log_filters = "INFO"
EOF
mkdir -p "$DATA/electrs/db"

echo "==> done. Next:"
echo "  scripts/regtest/start.sh        # start the stack"
echo "  scripts/regtest/stop.sh         # stop everything"
echo "  scripts/regtest/status.sh       # check what's running"
