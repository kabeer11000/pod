#!/usr/bin/env bash
# Starts a two-node cluster (rules + token auth) behind a pods router, plus a
# second router with the wrong cluster secret, and runs cluster.test.ts.
# Node "b" runs under a restart loop so the tests can kill it.
#
#   tests/e2e/run-cluster.sh /path/to/build/pod
set -u
BIN="${1:?usage: run-cluster.sh /path/to/pod}"
BASE="${BASE_PORT:-38200}"
# Not under /tmp: snap-packaged bun has a private /tmp and must read b.pid.
WORK="$(mktemp -d "${E2E_WORK_ROOT:-$HOME}/pod-e2e-XXXXXX")"
HERE="$(cd "$(dirname "$0")" && pwd)"

export POD_CLUSTER_SECRET_TEST="e2e-cluster-secret-0123456789"
export POD_AUTH_SECRET_TEST="e2e-auth-secret-0123456789"
A_PORT=$BASE A_INT=$((BASE + 1)) B_PORT=$((BASE + 2)) B_INT=$((BASE + 3))
R_PORT=$((BASE + 4)) RBAD_PORT=$((BASE + 5))

node_env() { # name port internal_port
  echo POD_PORT="$2" POD_INTERNAL_PORT="$3" POD_STORAGE_PATH="$WORK/$1" \
    POD_CLUSTER_SECRET="$POD_CLUSTER_SECRET_TEST" \
    POD_AUTH_SECRET="$POD_AUTH_SECRET_TEST" \
    POD_RULES_PATH="$HERE/rules.json" POD_LOG_LEVEL=warn
}

env $(node_env a $A_PORT $A_INT) "$BIN" >"$WORK/a.log" 2>&1 &
A_PID=$!
(
  while [ ! -f "$WORK/stop" ]; do
    env $(node_env b $B_PORT $B_INT) "$BIN" >>"$WORK/b.log" 2>&1 &
    echo $! >"$WORK/b.pid"
    wait $!
    sleep 0.2
  done
) &
LOOP_PID=$!
NODES="a=127.0.0.1:$A_INT,b=127.0.0.1:$B_INT"
POD_ROLE=router POD_PORT=$R_PORT POD_NODES="$NODES" POD_VIRTUAL_NODES=64 \
  POD_CLUSTER_SECRET="$POD_CLUSTER_SECRET_TEST" POD_LOG_LEVEL=info \
  "$BIN" >"$WORK/router.log" 2>&1 &
R_PID=$!
POD_ROLE=router POD_PORT=$RBAD_PORT POD_NODES="$NODES" \
  POD_CLUSTER_SECRET="this-is-the-wrong-secret" POD_LOG_LEVEL=warn \
  "$BIN" >"$WORK/router-bad.log" 2>&1 &
RBAD_PID=$!

cleanup() {
  touch "$WORK/stop"
  kill "$A_PID" "$R_PID" "$RBAD_PID" "$LOOP_PID" 2>/dev/null
  [ -f "$WORK/b.pid" ] && kill "$(cat "$WORK/b.pid")" 2>/dev/null
  wait 2>/dev/null
  for f in a b router router-bad; do
    echo "--- $f log ---"
    cat "$WORK/$f.log"
  done
  rm -rf "$WORK"
}
trap cleanup EXIT

for _ in $(seq 1 100); do
  status="$(curl -sf "http://127.0.0.1:$R_PORT/" 2>/dev/null)"
  [ "$(grep -c ' up,' <<<"$status")" = 2 ] && break
  sleep 0.1
done

cd "$HERE"
POD_CLUSTER_ROUTER="127.0.0.1:$R_PORT" POD_CLUSTER_ROUTER_BAD="127.0.0.1:$RBAD_PORT" \
  POD_CLUSTER_NODE_A="127.0.0.1:$A_PORT" POD_CLUSTER_NODE_B="127.0.0.1:$B_PORT" \
  POD_CLUSTER_B_PIDFILE="$WORK/b.pid" bun test cluster.test.ts
