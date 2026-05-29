#!/usr/bin/env bash
# Healthy once bootstrap wrote authkey and server supervisor marked Quack ready.
set -euo pipefail

WORK="${QUACKTAIL_WORK:-/work}"

test -f "${WORK}/authkey"
test -f "${WORK}/quack_ready"
