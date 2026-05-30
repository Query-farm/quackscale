# Headscale QuackTail e2e

Integration tests for a two-node QuackTail cluster over [Headscale](https://github.com/juanfont/headscale).

## Where tests live

| Test | How to run |
|------|------------|
| **Docker Compose demo** | [examples/README.md](../../examples/README.md) — `docker compose --profile test run --rm quacktail-client` |
| **GitHub Actions e2e** | [`.github/workflows/headscale-e2e.yml`](../../.github/workflows/headscale-e2e.yml) — manual `workflow_dispatch` |
| **Host script** | [`scripts/ci_headscale_e2e.sh`](../../scripts/ci_headscale_e2e.sh) — concurrent server + client containers |
| **Local host DuckDB** | [`scripts/local_remote_headscale_test.sh`](../../scripts/local_remote_headscale_test.sh) — join a running compose stack from the host |

All paths share the same client SQL shape (see [`scripts/lib/headscale_ci.sh`](../../scripts/lib/headscale_ci.sh) `headscale_ci_sql_client_session` and [`scripts/e2e/quacktail-compose-bootstrap.sh`](../../scripts/e2e/quacktail-compose-bootstrap.sh) `write_client_session_sql`).

## Server (`loopback_serve`)

Quack binds loopback; `tailscale_serve_local` publishes port 9494 on the tailnet:

```sql
CALL quack_serve('quack:127.0.0.1:9494', allow_other_hostname => true, token => quack_token());
CALL tailscale_serve_local(port => 9494);
```

Healthcheck: `/work/server.log` contains `quack:127.0.0.1:9494` and `local_forward`.

## Client (one DuckDB session, no curl)

```sql
LOAD quackscale;
CALL tailscale_up(...);
CALL tailscale_quack_forward(host => 'quacktail-server', port => 9494, local_port => 19494);
CALL tailscale_ping(host => 'quacktail-server', port => 9494);
LOAD quack;
CREATE SECRET (TYPE quack, TOKEN '…', SCOPE 'quack:127.0.0.1:19494');
FROM quack_query('quack:127.0.0.1:19494', 'SELECT 1 AS probe', ...);
ATTACH 'quack:127.0.0.1:19494' AS remote (TYPE quack);
SELECT * FROM remote.e2e_payload LIMIT 5;
SELECT 'PASSED' AS status, ... FROM remote.e2e_payload;
```

Invoked as: `duckdb -batch -echo -f client_session.sql` (in-memory; no `-bail` / `-init` file DB).

Compose waits for `quacktail-server` **healthy** before starting the client. The client retries the full session until a `PASSED` row appears.

## CI workflow

[`headscale-e2e.yml`](../../.github/workflows/headscale-e2e.yml):

1. Download release binary (`v1.0.2` by default)
2. Start Headscale in Docker
3. Run `scripts/ci_headscale_e2e.sh` (server container + client container)

## Debug probe

[`examples/docker-compose.yml`](../../examples/docker-compose.yml) profile `debug`: vanilla `tailscale/tailscale` container — isolates tailnet connectivity from DuckDB tsnet.
