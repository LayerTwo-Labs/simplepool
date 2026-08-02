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

# ---- pinned versions ----
# Pin upstream binaries so the stack's assumptions are explicit and any
# breakage maps to a deliberate bump of these lines, not an upstream
# "latest" moving underneath us. Bumping one IS the record that we
# revalidated against that version.
#
# The enforcer is the exception — it publishes no GitHub releases and
# releases.drivechain.info hosts only -latest- zips, so it CANNOT be
# URL-pinned. We record the version we validated against and warn (not
# fail) on drift; when the warning fires, revalidate and bump.
THUNDER_VERSION=0.16.1
BITCOIN_PATCHED_VERSION=v30.2
ENFORCER_VALIDATED_VERSION=v0.3.4

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

# Raw (non-zip) release asset, cached under a versioned name so a
# version bump invalidates the cache by construction.
fetch_bin() {
    local url="$1"
    local final="$2"    # canonical name we want in $BIN/
    local cache="$BIN/cache/$(basename "$url")"
    if [[ -f "$cache" ]]; then
        echo "  already have $(basename "$cache")"
    else
        echo "  downloading $(basename "$cache")"
        curl -fsSL -o "$cache.tmp" "$url"
        mv "$cache.tmp" "$cache"
    fi
    cp -f "$cache" "$BIN/$final"
    chmod +x "$BIN/$final"
}

extract_to_bin() {
    local zip="$1"
    local match="$2"    # glob (e.g. '*/bitcoind', '*bip300301-enforcer*')
    local final="$3"    # canonical name we want in $BIN/
    # Re-extract when the zip is newer than the binary — that's what a
    # version bump looks like (versioned zip name -> fresh download).
    if [[ -x "$BIN/$final" && "$BIN/$final" -nt "$zip" ]]; then
        echo "  $final already extracted"
        return
    fi
    # Extract into a scratch dir so the match is unambiguous — guessing
    # "the newest file in $BIN" breaks as soon as an unrelated binary
    # lives there. -j strips directories.
    local tmp
    tmp="$(mktemp -d)"
    unzip -qq -j -o "$zip" "$match" -d "$tmp"
    local files=("$tmp"/*)
    if [[ ${#files[@]} -ne 1 || ! -f "${files[0]}" ]]; then
        echo "expected exactly one file matching '$match' in $zip, got:" >&2
        ls "$tmp" >&2
        rm -rf "$tmp"
        exit 1
    fi
    mv -f "${files[0]}" "$BIN/$final"
    rm -rf "$tmp"
    chmod +x "$BIN/$final"
    # unzip preserves archive mtimes (possibly older than the zip);
    # stamp extraction time so the -nt freshness check above holds.
    touch "$BIN/$final"
}

# ---- download prebuilts ----
echo "==> fetching prebuilt binaries ($ARCH)"
mkdir -p "$BIN/cache"
# L1 node: versioned zips on releases.drivechain.info.
BITCOIN_ZIP_URL="https://releases.drivechain.info/L1-bitcoin-patched-${BITCOIN_PATCHED_VERSION}-${ARCH}.zip"
# Enforcer: only -latest- exists (see the pinned-versions note above).
ENFORCER_ZIP_URL="https://releases.drivechain.info/bip300301-enforcer-latest-${ARCH}.zip"
# Thunder: raw binaries attached to the thunder-rust GitHub release tag.
THUNDER_BASE_URL="https://github.com/LayerTwo-Labs/thunder-rust/releases/download/v${THUNDER_VERSION}"

fetch_zip "$BITCOIN_ZIP_URL"  "$BIN/bitcoind-${BITCOIN_PATCHED_VERSION}.zip"
fetch_zip "$ENFORCER_ZIP_URL" "$BIN/enforcer.zip"

echo "==> extracting binaries"
extract_to_bin "$BIN/bitcoind-${BITCOIN_PATCHED_VERSION}.zip" '*/bitcoind'     bitcoind
extract_to_bin "$BIN/bitcoind-${BITCOIN_PATCHED_VERSION}.zip" '*/bitcoin-cli'  bitcoin-cli
extract_to_bin "$BIN/enforcer.zip"  '*bip300301-enforcer*'    bip300301_enforcer
if [[ "$SKIP_THUNDER" != 1 ]]; then
    fetch_bin "$THUNDER_BASE_URL/thunder-${THUNDER_VERSION}-${ARCH}"     thunder
    fetch_bin "$THUNDER_BASE_URL/thunder-cli-${THUNDER_VERSION}-${ARCH}" thunder-cli
fi

# The enforcer can't be URL-pinned; surface drift loudly so a red CI run
# is attributable. Version line looks like: "bip300301_enforcer_lib v0.3.4".
ENFORCER_ACTUAL="$("$BIN/bip300301_enforcer" --version 2>/dev/null \
    | grep -m1 -oE 'v[0-9]+\.[0-9]+\.[0-9]+' || echo unknown)"
if [[ "$ENFORCER_ACTUAL" != "$ENFORCER_VALIDATED_VERSION" ]]; then
    echo "  WARNING: enforcer is $ENFORCER_ACTUAL, last validated" \
         "$ENFORCER_VALIDATED_VERSION — unpinnable upstream (latest-only" \
         "artifacts); revalidate and bump ENFORCER_VALIDATED_VERSION"
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
zmqpubrawblock=tcp://127.0.0.1:${REGTEST_BITCOIND_ZMQ_PORT:-29010}
zmqpubsequence=tcp://127.0.0.1:${REGTEST_BITCOIND_ZMQ_PORT:-29010}
[regtest]
rpcuser=user
rpcpassword=password
rpcport=${REGTEST_BITCOIND_RPC_PORT:-18443}
EOF

echo "==> done. Next:"
echo "  scripts/regtest/start.sh        # start the stack"
echo "  scripts/regtest/stop.sh         # stop everything"
echo "  scripts/regtest/status.sh       # check what's running"
