#!/usr/bin/env bash
# Full QuackTail e2e via examples/docker-compose.yml (same path as examples/README.md).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXAMPLES="$ROOT/examples"
LOG="${CI_COMPOSE_E2E_LOG:-${RUNNER_TEMP:-/tmp}/quacktail-compose-e2e.log}"

cd "$EXAMPLES"

echo "=== docker compose build (source) ==="
docker compose build quacktail-server quacktail-client

echo "=== verify image (attach_ducklake, tailscale_down, tailscale_quack_forward) ==="
docker compose run --rm --entrypoint /usr/local/bin/quacktail-verify-image.sh quacktail-client

echo "=== start headscale + quacktail-server ==="
docker compose up -d --force-recreate headscale quacktail-server

echo "=== run quacktail-client (profile test) ==="
: >"$LOG"
docker compose --profile test run --rm quacktail-client 2>&1 | tee "$LOG"

grep -q 'LAKE_PASSED' "$LOG" || {
  echo "error: LAKE_PASSED missing from client output" >&2
  exit 1
}
grep -q 'PASSED' "$LOG" || {
  echo "error: PASSED missing from client output" >&2
  exit 1
}
grep -qE 'Demo passed|CLIENT_DEMO_DONE' "$LOG" || {
  echo "error: demo completion marker missing" >&2
  exit 1
}
grep -q 'attach_ducklake' "$LOG" || {
  echo "error: attach_ducklake path not used (rebuild image from source?)" >&2
  exit 1
}

echo "ok: Headscale QuackTail compose e2e passed"
