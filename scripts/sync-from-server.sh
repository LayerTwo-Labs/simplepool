#!/usr/bin/env bash
#
# Pull the simplepool working tree from the test box (nakamoto) into this
# local checkout so we can commit + push from here.
#
# What it copies:
#   src/, include/, tests/, dashboard/{server.js,lib,public,views,*.json,*.md},
#   schema.sql, Makefile, README.md, proxy.conf.example, scripts/
#
# What it deliberately does NOT copy:
#   .git/             — keep local git history canonical
#   build/, *.o, *.d  — machine-specific compile output
#   data/             — runtime SQLite databases
#   node_modules/     — installed by `npm install` on each host
#   proxy.conf, .env  — may contain credentials
#
# Usage:
#   ./scripts/sync-from-server.sh            # dry run (default — shows diff)
#   ./scripts/sync-from-server.sh --apply    # actually copy the files
#
set -euo pipefail

REMOTE="${SIMPLEPOOL_REMOTE:-satoshi@192.168.50.154}"
REMOTE_PATH="${SIMPLEPOOL_REMOTE_PATH:-/home/satoshi/simplepool}"
SSH_KEY="${SIMPLEPOOL_SSH_KEY:-$HOME/.ssh/id_epitetus}"

# Resolve the local repo root (parent of this script's directory).
LOCAL_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

MODE="dry"
case "${1:-}" in
    --apply|-a) MODE="apply" ;;
    --dry|-n|"") MODE="dry" ;;
    -h|--help)
        sed -n '1,30p' "$0"; exit 0 ;;
    *)
        echo "unknown arg: $1" >&2; exit 2 ;;
esac

RSYNC_FLAGS=(
    -rlh                           # recursive, preserve symlinks; skip -t/-p
    --checksum                     # content-based diff (ignore mtime/perm noise)
    --itemize-changes
    -e "ssh -i $SSH_KEY -o StrictHostKeyChecking=accept-new"
    # Excludes
    --exclude='.git/'
    --exclude='.claude/'
    --exclude='build/'
    --exclude='data/'
    --exclude='*.o'
    --exclude='*.d'
    --exclude='node_modules/'
    --exclude='proxy.conf'
    --exclude='.env'
    --exclude='.env.local'
    --exclude='*.swp'
    --exclude='.DS_Store'
)

# We deliberately do NOT pass --delete. The server pulls from git, so the
# only divergence we want to capture is server-side EDITS that aren't yet
# committed. Anything local-only (this script, .claude/, empty include/)
# must be preserved. To force mirror semantics, pass --mirror.
if [[ "${2:-}" == "--mirror" || "${1:-}" == "--mirror" ]]; then
    RSYNC_FLAGS+=( --delete )
fi

if [[ "$MODE" == "dry" ]]; then
    RSYNC_FLAGS+=( --dry-run )
    echo "==> DRY RUN — no files will be written. Pass --apply to actually copy." >&2
fi

echo "==> rsync ${REMOTE}:${REMOTE_PATH}/  ->  ${LOCAL_ROOT}/" >&2
# Filter rsync's itemize output: drop the "no-op attribute drift" lines
# (leading '.' = no transfer needed) so we only show real content changes
# and deletions. Keep summary/status lines.
rsync "${RSYNC_FLAGS[@]}" "${REMOTE}:${REMOTE_PATH}/" "${LOCAL_ROOT}/" \
    | awk '/^[<>ch*]/ || /^\*deleting/ || NF==0 || /^sent / || /^total /'

if [[ "$MODE" == "dry" ]]; then
    echo "==> dry run complete." >&2
else
    echo "==> done. \`git status\` to review, then commit & push." >&2
fi
