#!/usr/bin/env bash
# Server healthy when authkey exists and Quack accepts POST on loopback (0.0.0.0 bind).
set -euo pipefail

WORK="${QUACKTAIL_WORK:-/work}"
PORT="${QUACK_PORT:-9494}"
TOKEN="${QUACK_TAILNET_TOKEN:-quackscale-demo-token}"

test -f "${WORK}/authkey"

code="$(curl -sS -m 3 -o /dev/null -w '%{http_code}' -X POST \
  -H "Authorization: Bearer ${TOKEN}" \
  -H 'Content-Type: application/json' \
  -d '{}' "http://127.0.0.1:${PORT}/quack" 2>/dev/null || echo 000)"
[[ "$code" != "000" ]]
