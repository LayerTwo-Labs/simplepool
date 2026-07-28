#!/usr/bin/env bash
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REGTEST="${REGTEST_DIR:-$ROOT/.regtest}"
RUN="$REGTEST/run"
LOGS="$REGTEST/logs"

for name in bitcoind bip300301_enforcer thunder; do
    pidfile="$RUN/$name.pid"
    if [[ -f "$pidfile" ]]; then
        pid="$(cat "$pidfile")"
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            printf "%-25s up   (pid %s, log %s)\n" "$name" "$pid" "$LOGS/$name.log"
            continue
        fi
    fi
    printf "%-25s down (log %s)\n" "$name" "$LOGS/$name.log"
done
