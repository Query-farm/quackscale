#!/usr/bin/env bash
# Thin wrapper around the CI hub smoke (two-process join, no Headscale).
exec "$(cd "$(dirname "$0")/../.." && pwd)/scripts/ci_hub_smoke.sh"
