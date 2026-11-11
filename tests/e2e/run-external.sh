#!/usr/bin/env bash
# Starts a pod binary on a throwaway port/storage dir, runs the e2e suite
# against it, then stops it. Useful when the test runner cannot spawn the
# binary itself (e.g. snap-packaged bun, sanitizer builds).
#
#   tests/e2e/run-external.sh /path/to/build/pod
set -u
BIN="${1:?usage: run-external.sh /path/to/pod}"
PORT="${PORT:-38123}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

POD_PORT="$PORT" POD_STORAGE_PATH="$WORK/db" DEFAULT_NAMESPACE=fallback \
  POD_LOG_LEVEL=warn "$BIN" >"$WORK/server.log" 2>&1 &
PID=$!

for _ in $(seq 1 100); do
  curl -sf "http://127.0.0.1:$PORT/" >/dev/null 2>&1 && break
  sleep 0.1
done

cd "$(dirname "$0")"
POD_EXTERNAL_PORT="$PORT" bun test
STATUS=$?

kill -TERM "$PID" 2>/dev/null
wait "$PID" 2>/dev/null
echo "--- server log ---"
cat "$WORK/server.log"
exit $STATUS
