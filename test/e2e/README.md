# Headscale QuackTail e2e

Headscale stays up; **server** and **client** DuckDB containers run concurrently.

## Server bind mode (default: `loopback_serve`)

With tsnet embedded **inside DuckDB**, Quack must listen on loopback and **`tailscale_serve_local`** publishes port 9494 on the tailnet (see [examples/headscale_quacktail.sql](../../examples/headscale_quacktail.sql)). A direct `quack:0.0.0.0` bind only satisfies loopback health checks — cross-node ATTACH hangs.

```sql
CALL quack_serve('quack:127.0.0.1:9494', allow_other_hostname => true, token => quack_token());
CALL tailscale_serve_local(port => 9494);
```

Experimental direct bind: `E2E_QUACK_SERVE_MODE=direct`.

## Client

- ATTACH URI matches CI: `quack:quacktail-server:9494` (`E2E_QUACK_ATTACH_HOST=hostname`)
- `/etc/hosts` maps `quacktail-server` → server tailnet IP (`--add-host` in CI, entrypoint in compose)

## Flow

1. `docker run -d` server → `tailscale_up` → `quack_serve(127.0.0.1)` → `tailscale_serve_local`
2. Resolve server IP from Headscale
3. `docker run -d` client (while server runs) → `tailscale_up` → ATTACH hostname (hosts → tailnet IP)
