#!/usr/bin/env bash
#
# Record where a binary came from, at the moment it is built.
#
# simplepool and the bip300301 enforcer embed their build commit and print it
# from --version, so they need nothing. thunder and bitcoind print a version
# number and no commit, and the only other place the commit exists is the
# source tree — which means the dashboard would have to keep a full checkout of
# every dependency on the production box forever just to answer "which commit
# is this?". That is the wrong dependency: source is a build-time requirement,
# not a runtime one.
#
# So the commit is captured here, while the source is still around, into a
# small JSON file beside the binary. It is pinned to the binary by sha256, so a
# rebuild without a re-record is detected rather than silently misreported.
# Afterwards the checkout can be deleted and /api/versions still answers.
#
# Usage:
#   scripts/record-build.sh <component> <repo-dir> <binary>
#
#   scripts/record-build.sh thunder \
#       ~/forknet-software/thunder-rust \
#       ~/forknet-software/thunder-rust/target/release/thunder_app
#
# Writes <binary>.build.json. Run it immediately after every build of a
# component that does not embed its own commit — put it in the same script or
# Makefile target that does the build, so the two can't drift apart.
#
# Options:
#   --out <path>   write somewhere other than <binary>.build.json
#
set -euo pipefail

OUT=""
ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="${2:?--out needs a path}"; shift 2 ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) ARGS+=("$1"); shift ;;
    esac
done

if [ "${#ARGS[@]}" -ne 3 ]; then
    echo "usage: $0 <component> <repo-dir> <binary> [--out <path>]" >&2
    exit 2
fi

COMPONENT="${ARGS[0]}"
REPO_DIR="${ARGS[1]}"
BINARY="${ARGS[2]}"
OUT="${OUT:-${BINARY}.build.json}"

[ -d "$REPO_DIR" ] || { echo "record-build: no such repo dir: $REPO_DIR" >&2; exit 1; }
[ -f "$BINARY" ]   || { echo "record-build: no such binary: $BINARY" >&2; exit 1; }

git -C "$REPO_DIR" rev-parse --git-dir >/dev/null 2>&1 || {
    echo "record-build: $REPO_DIR is not a git checkout" >&2; exit 1; }

COMMIT=$(git -C "$REPO_DIR" rev-parse HEAD)
BRANCH=$(git -C "$REPO_DIR" rev-parse --abbrev-ref HEAD)
CTIME=$(git -C "$REPO_DIR" show -s --format=%cI HEAD)
SUBJECT=$(git -C "$REPO_DIR" show -s --format=%s HEAD)
REMOTE=$(git -C "$REPO_DIR" config --get remote.origin.url || echo "")

# Tracked modifications only — untracked build output is not a source change.
# Recorded rather than refused: building from a dirty tree is a legitimate
# thing to do in a hurry, it just has to be visible afterwards.
if [ -n "$(git -C "$REPO_DIR" status --porcelain --untracked-files=no)" ]; then
    DIRTY=true
else
    DIRTY=false
fi

# Pins this record to the exact artifact. Without it the file would still claim
# to describe a binary that has since been rebuilt.
if command -v sha256sum >/dev/null 2>&1; then
    SHA=$(sha256sum "$BINARY" | cut -d' ' -f1)
else
    SHA=$(shasum -a 256 "$BINARY" | cut -d' ' -f1)      # macOS
fi

SIZE=$(wc -c < "$BINARY" | tr -d ' ')
NOW=$(date +%s)

# Escape the one field that is free-form text.
SUBJECT_JSON=$(printf '%s' "$SUBJECT" | sed 's/\\/\\\\/g; s/"/\\"/g')

cat > "$OUT" <<JSON
{
  "component":     "$COMPONENT",
  "repo":          "$REMOTE",
  "branch":        "$BRANCH",
  "commit":        "$COMMIT",
  "commit_time":   "$CTIME",
  "subject":       "$SUBJECT_JSON",
  "dirty":         $DIRTY,
  "binary":        "$(basename "$BINARY")",
  "binary_sha256": "$SHA",
  "binary_size":   $SIZE,
  "recorded_at":   $NOW,
  "recorded_on":   "$(hostname)"
}
JSON

echo "record-build: $COMPONENT ${COMMIT:0:7} ($BRANCH)$([ "$DIRTY" = true ] && echo ' [dirty]') -> $OUT"
