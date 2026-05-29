#!/usr/bin/env bash
# QuackTail CI container entrypoint — server keeps quack_serve in the foreground.
set -euo pipefail

DUCKDB="${DUCKDB_BIN:-/usr/local/bin/duckdb}"
ROLE="${QUACKTAIL_ROLE:-${1:-server}}"
PORT="${QUACK_PORT:-9494}"
WORK="${QUACKTAIL_WORK:-/work}"
DB="${WORK}/server.duckdb"

if [[ ! -x "$DUCKDB" ]]; then
  echo "error: DuckDB not found or not executable at $DUCKDB" >&2
  exit 1
fi

ensure_quack() {
  echo "=== ensure quack extension ==="
  if "$DUCKDB" :memory: -batch -c "LOAD quack; SELECT 1;"; then
    return 0
  fi
  echo "Installing quack from DuckDB core ..."
  "$DUCKDB" :memory: -batch -c "INSTALL quack FROM core; LOAD quack; SELECT 1;" \
    || "$DUCKDB" :memory: -batch -c "INSTALL quack FROM core_nightly; LOAD quack; SELECT 1;"
}

run_server() {
  ensure_quack
  echo "=== server setup (tailscale + seed) ==="
  cat "${WORK}/server_setup.sql"
  "$DUCKDB" "$DB" -batch -echo -f "${WORK}/server_setup.sql"
  echo "=== quack_serve (foreground; container stays alive) ==="
  exec "$DUCKDB" "$DB" -batch -echo -c "
LOAD quack;
CALL quack_serve(
    'quack:0.0.0.0:${PORT}',
    allow_other_hostname => true,
    token => quack_token()
);
"
}

run_client() {
  ensure_quack
  echo "=== client SQL ==="
  cat "${WORK}/client.sql"
  exec "$DUCKDB" :memory: -batch -echo -f "${WORK}/client.sql"
}

case "$ROLE" in
  server) run_server ;;
  client) run_client ;;
  *)
    echo "error: unknown QUACKTAIL_ROLE '$ROLE' (expected server or client)" >&2
    exit 1
    ;;
esac
