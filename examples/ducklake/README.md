# DuckLake + Quack on QuackTail

Branch **`ducklake`** extends the compose demo: the server attaches a local DuckLake catalog, seeds an `inventory` table, then `quack_serve` exposes it on the tailnet. The client discovers and queries the lake **via `quack_query`** (SQL runs on the server where DuckLake is attached).

## Architecture

```text
quacktail-server                          quacktail-client
─────────────────                         ─────────────────
tailscale_up                              tailscale_up
ATTACH ducklake:… AS lake (local Parquet)  tailscale_quack_forward
  └─ ducklake-lake volume                   quack_query → quack_discover()  (find)
quack_serve(127.0.0.1:9494)               quack_query → lake.inventory     (query)
tailscale_serve_local                     ATTACH quack:… AS remote (e2e)
```

Parquet + metadata live on **`ducklake-lake`** on the server only (`/var/lib/ducklake`).

## Access patterns

| Pattern | When to use |
|---------|-------------|
| **`quack_query(uri, '…')`** | Server owns DuckLake files (compose demo). Find + query without client-side Parquet. |
| **`ATTACH 'ducklake:quack:…' AS lake (DATA_PATH '…')`** | Client has local or shared Parquet path ([DuckDB 1.5.3](https://duckdb.org/2026/05/20/announcing-duckdb-153.html)). |
| **`ATTACH 'quack:…' AS remote`** | Primary catalog only (`remote.e2e_payload`). **Not** nested `remote.lake.*`. |

## Run the demo

```bash
cd examples
docker compose build quacktail-server quacktail-client
docker compose up -d --force-recreate headscale quacktail-server
docker compose --profile test run --rm quacktail-client
```

Expect `PASSED` (Quack e2e) and `LAKE_PASSED` with `inventory_rows = 2`.

## Tailnet client SQL

```sql
CALL tailscale_quack_forward(host => 'quacktail-server', port => 9494, local_port => 19494);
CREATE SECRET (TYPE quack, TOKEN 'quackscale-demo-token', SCOPE 'quack:127.0.0.1:19494');

-- Find: run quack_discover on the server (not locally — local discover lists this node only)
FROM quack_query('quack:127.0.0.1:19494', 'FROM quack_discover()', token => '…', disable_ssl => true);

-- Query: SQL executes on server where lake is attached
FROM quack_query('quack:127.0.0.1:19494', 'SELECT * FROM lake.inventory', token => '…', disable_ssl => true);
```

See [docs/DUCKLAKE_TAILNET.md](../docs/DUCKLAKE_TAILNET.md) and [local-demo.sql](local-demo.sql).
