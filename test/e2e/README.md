# Headscale QuackTail e2e tests

Two DuckDB containers join **Headscale** via `tailscale_up` (tsnet). Quack runs on the **tailnet** — no Docker DNS shortcuts.

## Flow

1. **Server:** `tailscale_up` → `CALL quack_serve(quack_uri(), allow_other_hostname => true, token => quack_token())`
2. **Client:** `tailscale_up` → `quack_discover()` → `ATTACH 'quack:<server>.<magicdns>:9494'` with `DISABLE_SSL true`

Same pattern as [docs/PLAN.md](../docs/PLAN.md) and [examples/headscale_quacktail.sql](../examples/headscale_quacktail.sql).

Containers use `NET_ADMIN` + `/dev/net/tun` (like the Tailscale verify step). Server readiness is checked over the **tailnet IP**, not loopback.

## Env overrides

| Variable | Default | Purpose |
|----------|---------|---------|
| `E2E_QUACK_ATTACH_HOST` | `magicdns` | Client ATTACH URI (`ip` → tailnet IP) |
| `E2E_QUACK_SERVE_URI` | `tailnet` | Use `quack_uri()` (`0.0.0.0` to force bind-all) |
| `E2E_TAILNET_MESH_WAIT_SEC` | `15` | Pause after both nodes register |

## Run

```sh
eval "$(./scripts/ci_download_release_duckdb.sh latest)"
./scripts/ci_headscale_e2e.sh
```

GitHub Actions: **Headscale QuackTail e2e** (`workflow_dispatch`).
