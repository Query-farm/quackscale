# QuackScale documentation

## Start here

1. **[usage.md](usage.md)** — **use cases & solution patterns** (Quack, DuckLake, S3, discovery)  
2. **[../README.md](../README.md)** — build, quick start, SQL reference  
3. **[AUTHENTICATION.md](AUTHENTICATION.md)** — Tailscale (`TS_AUTHKEY`, `tailscale_up`, browser login)  
4. **[HEADSCALE.md](HEADSCALE.md)** — self-hosted [Headscale](https://github.com/juanfont/headscale) (`control_url`, preauth keys)  
5. **[QUACK_AUTH.md](QUACK_AUTH.md)** — Quack tokens for QuackTail (`QUACK_TAILNET_TOKEN`, shared secrets, auth macros)  
6. **[DUCKLAKE_TAILNET.md](DUCKLAKE_TAILNET.md)** — DuckLake over the tailnet (compose demo, patterns B/C)  
7. **[DUCKLAKE_REMOTE_ATTACH.md](DUCKLAKE_REMOTE_ATTACH.md)** — transparent remote lake reads (`attach_ducklake`)  
8. **[PLAN.md](PLAN.md)** — architecture, API roadmap, risks  
9. **[../examples/README.md](../examples/README.md)** — Docker Compose two-node Headscale demo  

## QuackTail authentication at a glance

| Step | Layer | Action |
|------|--------|--------|
| 1 | Tailscale | `export TS_AUTHKEY=...` → `CALL tailscale_up(hostname => 'node-a', ...)` |
| 2 | Quack (server) | `export QUACK_TAILNET_TOKEN=...` → `CALL quack_serve(..., token => quack_token())` + `tailscale_serve_local` |
| 3 | Quack (client) | `CALL tailscale_quack_forward(...)` → `CREATE SECRET` → `ATTACH 'quack:127.0.0.1:19494'` |

Do **not** rely on the random `auth_token` column from default `quack_serve`. Use a **shared** token or [override `quack_authentication_function`](https://duckdb.org/docs/current/quack/security#overriding-authentication).
