# Headscale QuackTail e2e

Headscale stays up; **server** and **client** DuckDB containers run concurrently.

## Server bind mode (default: `tailnet`)

Per [Quack docs](https://duckdb.org/docs/current/quack/overview), bind **`quack:0.0.0.0:9494`** (tsnet tailnet IPs are not bindable). Clients ATTACH via hostname + `/etc/hosts` or tailnet IP with `allow_other_hostname => true` on the server.

```sql
CALL quack_serve('quack:0.0.0.0:9494', allow_other_hostname => true, token => quack_token());
```

Legacy loopback + Serve: `E2E_QUACK_SERVE_MODE=loopback_serve`.

## Client

- ATTACH URI matches server: `quack:quacktail-server:9494` (`E2E_QUACK_ATTACH_HOST=hostname`)
- `/etc/hosts` maps `quacktail-server` → server tailnet IP
- `quack_query('SELECT 1')` probe runs before ATTACH (same HTTP stack)

## Flow

1. `docker run -d` server → `tailscale_up` → `quack_serve(quack_uri(), ...)`
2. Resolve server IP from Headscale
3. `docker run -d` client (while server runs) → `tailscale_up` → probe → ATTACH
