#!/usr/bin/env bash
# Spin up a local BIP300 regtest stack to validate the pool's drivechain
# coinbase shape end-to-end.
#
# Stack:
#   bitcoind-patched   — BIP300/301-aware Bitcoin Core fork (LayerTwo-Labs)
#   bip300301_enforcer — validator that watches the BTC chain for deposits
#   thunder            — the L2-S9 sidechain node (skippable, see below)
#
# No electrs: the enforcer wallet runs with --wallet-sync-source=disabled,
# which keeps the wallet in sync purely from incoming blocks — exactly
# right for a from-genesis regtest chain.
#
# State lives under .regtest/ (gitignored). Override with REGTEST_DIR.
#
# Env:
#   REGTEST_DIR           where chain state/logs live (default: <repo>/.regtest)
#   REGTEST_BIN_DIR       binary cache — zips + extracted binaries — so data
#                         can be wiped/relocated without re-downloading
#                         (default: $REGTEST_DIR/bin)
#   REGTEST_SKIP_THUNDER  =1 to skip the thunder download (CI does this;
#                         thunder plays no part in the coinbase-shape e2e)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REGTEST="${REGTEST_DIR:-$ROOT/.regtest}"
BIN="${REGTEST_BIN_DIR:-$REGTEST/bin}"
DATA="$REGTEST/data"
LOGS="$REGTEST/logs"
SKIP_THUNDER="${REGTEST_SKIP_THUNDER:-0}"
mkdir -p "$BIN" "$DATA/bitcoind" "$DATA/enforcer" "$DATA/thunder" "$LOGS"

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
    # -j strips directories. Some zips have only one file.
    unzip -qq -j -o "$zip" "$match" -d "$BIN"
    # Find the most-recently-extracted file and rename to $final if needed.
    # Exclude already-canonical filenames so each extraction is idempotent
    # regardless of what got extracted before it.
    local actual
    actual="$(ls -t "$BIN" | grep -v -E '\.zip$|^(bitcoind|bitcoin-cli|bip300301_enforcer|thunder|thunder-cli)$' | head -1 || true)"
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
        THUNDER_ZIP_URL="https://releases.drivechain.info/L2-S9-Thunder-latest-aarch64-apple-darwin.zip"
        ;;
    x86_64-apple-darwin)
        BITCOIN_ZIP_URL="https://releases.drivechain.info/L1-bitcoin-patched-latest-x86_64-apple-darwin.zip"
        ENFORCER_ZIP_URL="https://releases.drivechain.info/bip300301-enforcer-latest-x86_64-apple-darwin.zip"
        THUNDER_ZIP_URL="https://releases.drivechain.info/L2-S9-Thunder-latest-x86_64-apple-darwin.zip"
        ;;
    x86_64-unknown-linux-gnu)
        BITCOIN_ZIP_URL="https://releases.drivechain.info/L1-bitcoin-patched-latest-x86_64-unknown-linux-gnu.zip"
        ENFORCER_ZIP_URL="https://releases.drivechain.info/bip300301-enforcer-latest-x86_64-unknown-linux-gnu.zip"
        THUNDER_ZIP_URL="https://releases.drivechain.info/L2-S9-Thunder-latest-x86_64-unknown-linux-gnu.zip"
        ;;
    *)
        echo "no prebuilt binaries for $ARCH — build from source" >&2
        exit 1
        ;;
esac

fetch_zip "$BITCOIN_ZIP_URL"  "$BIN/bitcoind.zip"
fetch_zip "$ENFORCER_ZIP_URL" "$BIN/enforcer.zip"
[[ "$SKIP_THUNDER" == 1 ]] || fetch_zip "$THUNDER_ZIP_URL" "$BIN/thunder.zip"

echo "==> extracting binaries"
extract_to_bin "$BIN/bitcoind.zip"  '*/bitcoind'                 bitcoind
extract_to_bin "$BIN/bitcoind.zip"  '*/bitcoin-cli'              bitcoin-cli
extract_to_bin "$BIN/enforcer.zip"  '*bip300301-enforcer*'       bip300301_enforcer
if [[ "$SKIP_THUNDER" != 1 ]]; then
    extract_to_bin "$BIN/thunder.zip"   'thunder-latest-*'       thunder
    extract_to_bin "$BIN/thunder.zip"   'thunder-cli-latest-*'   thunder-cli
fi

echo "==> binaries ready in $BIN"
ls -la "$BIN" | tail -n +2

# ---- write configs ----
echo "==> writing configs"

cat > "$DATA/bitcoind/bitcoin.conf" <<EOF
regtest=1
server=1
# No P2P: single-node stack, and bitcoind's default regtest P2P port
# (18444) is the same one the enforcer's GBT server binds. macOS lets a
# specific-IP bind coexist with a wildcard listener, Linux does not —
# with listen=1 the enforcer dies with EADDRINUSE on CI.
listen=0
txindex=1
rest=1
fallbackfee=0.0001
# ZMQ options are global-only: inside the [regtest] section bitcoind
# parses but silently ignores them (getzmqnotifications returns []),
# and the enforcer's validator then never advances past startup tip.
# Port 29010, not the conventional 29000 — a ZMQ bind conflict with
# another local node is silent, and the enforcer will happily subscribe
# to whichever process won the port.
zmqpubrawblock=tcp://127.0.0.1:29010
zmqpubsequence=tcp://127.0.0.1:29010
[regtest]
rpcuser=user
rpcpassword=password
rpcport=18443
EOF

echo "==> done. Next:"
echo "  scripts/regtest/start.sh        # start the stack"
echo "  scripts/regtest/stop.sh         # stop everything"
echo "  scripts/regtest/status.sh       # check what's running"
