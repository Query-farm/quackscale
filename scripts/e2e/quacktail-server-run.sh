#!/usr/bin/env bash
# Long-lived server: DuckDB init (tailscale + quack_serve) then touch /work/quack_ready.
set -euo pipefail

# shellcheck source=/dev/null
source /usr/local/lib/quacktail_ext.sh

DUCKDB="${DUCKDB_BIN:-/usr/local/bin/duckdb}"
WORK="${QUACKTAIL_WORK:-/work}"
DB="${WORK}/server.duckdb"
INIT_SQL="${WORK}/server_init.sql"
PORT="${QUACK_PORT:-9494}"
TOKEN="${QUACK_TAILNET_TOKEN:-quackscale-demo-token}"
READY="${WORK}/quack_ready"
LOG="${WORK}/server.log"
WAIT_SEC="${QUACKTAIL_SERVER_READY_SEC:-120}"

rm -f "$READY"
: >"$LOG"

sleep infinity | stdbuf -oL -eL "$DUCKDB" -bail -batch \
  -cmd "SET extension_directory='${DUCKDB_EXTENSION_DIRECTORY:-/duckdb_extensions}';" \
  "$DB" -init "$INIT_SQL" >>"$LOG" 2>&1 &
duck_pid=$!

cleanup() {
  kill "$duck_pid" 2>/dev/null || true
  wait "$duck_pid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

for ((i = 1; i <= WAIT_SEC; i++)); do
  if ! kill -0 "$duck_pid" 2>/dev/null; then
    echo "error: server DuckDB exited during init (see ${LOG})" >&2
    tail -40 "$LOG" >&2 || true
    wait "$duck_pid" || true
    exit 1
  fi
  if quacktail_quack_endpoint_ready "127.0.0.1" "$PORT" "$TOKEN"; then
    touch "$READY"
    break
  fi
  sleep 1
done

if [[ ! -f "$READY" ]]; then
  echo "error: Quack not listening on 127.0.0.1:${PORT} after ${WAIT_SEC}s (see ${LOG})" >&2
  tail -40 "$LOG" >&2 || true
  exit 1
fi

wait "$duck_pid"
