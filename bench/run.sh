#!/usr/bin/env bash
# Starts a pod binary with a fresh storage directory, runs a command against
# it, then stops the server.
#
#   bench/run.sh ./build/pod bun bench/pod-bench.ts
#   bench/run.sh ./build/pod bun examples/web/demo.ts
set -u
BIN="${1:?usage: run.sh /path/to/pod <command...>}"
shift
PORT="${PORT:-3000}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

POD_PORT="$PORT" POD_STORAGE_PATH="$WORK/db" POD_LOG_LEVEL="${POD_LOG_LEVEL:-warn}" \
  "$BIN" >"$WORK/server.log" 2>&1 &
PID=$!
for _ in $(seq 1 100); do
  curl -sf "http://127.0.0.1:$PORT/" >/dev/null 2>&1 && break
  sleep 0.1
done

POD_URL="ws://127.0.0.1:$PORT/webchannel" "$@"
STATUS=$?

kill -TERM "$PID" 2>/dev/null
wait "$PID" 2>/dev/null
if [ -s "$WORK/server.log" ]; then
  echo "--- server log ---"
  cat "$WORK/server.log"
fi
exit $STATUS
