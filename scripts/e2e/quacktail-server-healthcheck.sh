#!/usr/bin/env bash
# Healthy once bootstrap wrote authkey, DuckDB stayed up, and server registered on Headscale.
set -euo pipefail

WORK="${QUACKTAIL_WORK:-/work}"
SERVER_HOST="${SERVER_HOST:-quacktail-server}"
HS_CFG="${HEADSCALE_CONFIG:-/etc/headscale/config.yaml}"

test -f "${WORK}/authkey"
test -f "${WORK}/quack_ready"

if command -v headscale >/dev/null 2>&1; then
  headscale -c "$HS_CFG" nodes list 2>/dev/null | grep -Fq "$SERVER_HOST"
fi
