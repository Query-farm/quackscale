# Headscale QuackTail e2e tests

Two DuckDB containers join **Headscale** via `tailscale_up`. Quack listens on **loopback**; **Tailscale Serve** exposes it on the tailnet.

## Flow

1. **Server:** `tailscale_up` → `quack_serve('quack:127.0.0.1:9494', allow_other_hostname => true, …)` → `CALL tailscale_serve_local(port => 9494)`
2. **Client:** one DuckDB session — `tailscale_up` → `ATTACH 'quack:<server-tailnet-ip>:9494'` → queries (tsnet stays up for the whole session)

Quack stays local; quackscale uses libtailscale `SetServeConfig` TCP forward (same idea as `tailscale serve --tcp=9494 localhost:9494`).

## Env overrides

| Variable | Default | Purpose |
|----------|---------|---------|
| `E2E_QUACK_ATTACH_HOST` | `ip` | Client URI (`magicdns` when tsnet accepts tailnet DNS) |
| `E2E_TAILNET_MESH_WAIT_SEC` | `15` | Pause after server ready, before client container starts |

## Run

```sh
eval "$(./scripts/ci_download_release_duckdb.sh latest)"
./scripts/ci_headscale_e2e.sh
```

GitHub Actions: **Headscale QuackTail e2e** (`workflow_dispatch`, `release_tag` defaults to latest). The release must include `tailscale_serve_local`.
