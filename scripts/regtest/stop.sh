#!/usr/bin/env bash
# Stop the regtest stack. Sends SIGTERM, waits up to 5s, then SIGKILL.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN="$ROOT/.regtest/run"

for name in thunder bip300301_enforcer electrs bitcoind; do
    pidfile="$RUN/$name.pid"
    [[ -f "$pidfile" ]] || continue
    pid="$(cat "$pidfile")"
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        echo "  stopping $name (pid $pid)"
        kill "$pid" 2>/dev/null || true
        for ((i = 0; i < 5; i++)); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 1
        done
        kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pidfile"
done
echo "stopped."
