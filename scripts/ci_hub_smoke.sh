#!/usr/bin/env bash
# Two-process hub smoke: this DuckDB hosts the control plane; a second process joins.
# Analog of scripts/ci_headscale_smoke.sh (no Headscale container).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB="${DUCKDB:-${DUCKDB_BIN:-$ROOT/build/release/duckdb}}"

# DuckDB's default box renderer ends with a border line (└────┘). Always use CSV.
hub_ci_linked() {
  local out
  out="$("${DUCKDB}" :memory: -unsigned -bail -batch -csv -noheader -c \
    "LOAD quackscale; SELECT linked FROM quackscale_status();")"
  printf '%s\n' "${out}" | { grep -E '^(true|false)$' || true; } | tail -1 | tr -d '\r'
}

if [[ "${1:-}" == "--assert-linked" ]]; then
  if [[ ! -x "${DUCKDB}" ]]; then
    echo "error: DuckDB not found at ${DUCKDB} (set DUCKDB or DUCKDB_BIN)" >&2
    exit 1
  fi
  linked="$(hub_ci_linked)"
  echo "quackscale_status.linked=${linked}"
  if [[ "${linked}" != "true" ]]; then
    echo "error: this DuckDB was built without the hub" >&2
    echo "checkout wirebone.cpp into third_party/wirebone (or set QUACKSCALE_WIREBONE_DIR) and rebuild with OpenSSL, nghttp2, and libzstd." >&2
    exit 1
  fi
  exit 0
fi

LISTEN="${HUB_LISTEN:-127.0.0.1:18080}"
URL="http://${LISTEN}"
WORK="$(mktemp -d /tmp/quackscale-hub-XXXXXX)"
LOG_DIR="${HUB_CI_LOG_DIR:-$ROOT/.e2e-work/hub-smoke}"
COORD_DB="${WORK}/coord.duckdb"
COORD_INIT="${WORK}/coord_init.sql"
COORD_LOG="${WORK}/coord.log"
PEER_LOG="${WORK}/peer.log"
KEY_FILE="${WORK}/preauth.key"
WAIT_SEC="${HUB_READY_SEC:-60}"

copy_logs() {
  mkdir -p "${LOG_DIR}"
  cp -f "${COORD_LOG}" "${PEER_LOG}" "${COORD_INIT}" "${LOG_DIR}/" 2>/dev/null || true
}

# Hub DuckDB is kept alive with an open stdin pipe and may be running tsnet, which
# ignores SIGTERM. Kill the whole session (TERM then KILL) so CI cannot hang on wait.
stop_hub() {
  local pid="${1:-}"
  [[ -n "${pid}" ]] || return 0
  kill -TERM -- "-${pid}" 2>/dev/null || kill -TERM "${pid}" 2>/dev/null || true
  local i
  for i in 1 2 3 4 5 6 7 8; do
    kill -0 "${pid}" 2>/dev/null || return 0
    sleep 0.25
  done
  kill -KILL -- "-${pid}" 2>/dev/null || kill -KILL "${pid}" 2>/dev/null || true
  wait "${pid}" 2>/dev/null || true
}

cleanup() {
  local code=$?
  trap - EXIT INT TERM
  copy_logs
  stop_hub "${COORD_PID:-}"
  rm -rf "${WORK}"
  exit "${code}"
}
trap cleanup EXIT INT TERM

if [[ ! -x "${DUCKDB}" ]]; then
  echo "error: DuckDB not found at ${DUCKDB} (set DUCKDB or DUCKDB_BIN)" >&2
  exit 1
fi

linked="$(hub_ci_linked)"
if [[ "${linked}" != "true" ]]; then
  echo "error: this DuckDB was built without the hub (quackscale_status.linked=${linked})" >&2
  echo "checkout wirebone.cpp into third_party/wirebone (or set QUACKSCALE_WIREBONE_DIR) and rebuild with OpenSSL, nghttp2, and libzstd." >&2
  exit 1
fi

cat >"${COORD_INIT}" <<SQL
LOAD quackscale;
CALL quackscale_hub(
    hostname   => 'coord',
    listen     => '${LISTEN}',
    server_url => '${URL}',
    dns_listen => '',
    join       => false,
    state_dir  => '${WORK}/coord-ts'
);
COPY (SELECT preauth_key FROM quackscale_status()) TO '${KEY_FILE}' (FORMAT csv, HEADER false);
SQL

echo "Joining in-process hub from a second DuckDB (control_url=${URL}) ..."
echo "→ hub ${URL} (state ${WORK})"
# New session so cleanup can SIGKILL tsnet threads that ignore TERM.
HUB_DUCKDB="${DUCKDB}" HUB_DB="${COORD_DB}" HUB_INIT="${COORD_INIT}" \
  setsid sh -c 'sleep infinity | exec "$HUB_DUCKDB" -unsigned -bail -batch "$HUB_DB" -init "$HUB_INIT"' \
  >"${COORD_LOG}" 2>&1 &
COORD_PID=$!

ready=0
for ((i = 1; i <= WAIT_SEC; i++)); do
  if ! kill -0 "${COORD_PID}" 2>/dev/null; then
    echo "error: hub DuckDB exited (see ${COORD_LOG})" >&2
    tail -40 "${COORD_LOG}" >&2 || true
    exit 1
  fi
  if [[ -s "${KEY_FILE}" ]] && curl -sf "${URL}/healthz" >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 1
done

if [[ "${ready}" != 1 ]]; then
  echo "error: hub did not become ready within ${WAIT_SEC}s (see ${COORD_LOG})" >&2
  tail -40 "${COORD_LOG}" >&2 || true
  exit 1
fi

KEY="$(tr -d '[:space:]' <"${KEY_FILE}")"
echo "→ bootstrap ${KEY}"
echo "--- SQL ---"
cat <<SQL
CALL tailscale_up(
    hostname    => 'peer',
    control_url => '${URL}',
    authkey     => '${KEY}',
    state_dir   => '${WORK}/peer-ts',
    ephemeral   => true
);
CALL tailscale_status();
CALL tailscale_down();
SQL
echo "--- DuckDB output ---"

"${DUCKDB}" -unsigned -bail -c "
LOAD quackscale;
CALL tailscale_up(
    hostname    => 'peer',
    control_url => '${URL}',
    authkey     => '${KEY}',
    state_dir   => '${WORK}/peer-ts',
    ephemeral   => true
);
CALL tailscale_status();
CALL tailscale_down();
" | tee "${PEER_LOG}" || {
  echo "error: peer failed to join (hub log follows)" >&2
  tail -40 "${COORD_LOG}" >&2 || true
  exit 1
}

if ! grep -q 'true' "${PEER_LOG}"; then
  echo "error: peer tailscale_status did not report running" >&2
  exit 1
fi

echo "Hub + QuackTail smoke test passed."
