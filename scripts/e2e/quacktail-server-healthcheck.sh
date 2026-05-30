#!/usr/bin/env bash
# Healthy once bootstrap wrote authkey, server init logged quack_serve + serve_local, node on tailnet.
set -euo pipefail

WORK="${QUACKTAIL_WORK:-/work}"
SERVER_HOST="${SERVER_HOST:-quacktail-server}"
PORT="${QUACK_PORT:-9494}"
HS_CFG="${HEADSCALE_CONFIG:-/etc/headscale/config.yaml}"

# shellcheck source=/dev/null
source /usr/local/lib/quacktail_ext.sh

test -f "${WORK}/authkey"
quacktail_server_log_ready "${WORK}/server.log" "$PORT" "$SERVER_HOST"

if command -v headscale >/dev/null 2>&1; then
  headscale -c "$HS_CFG" nodes list 2>/dev/null | grep -Fq "$SERVER_HOST"
fi
