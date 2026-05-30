# DuckLake + Quack on QuackTail

The compose demo on branch **`ducklake`**: the server attaches a local DuckLake catalog, seeds `inventory`, and exposes it on the tailnet. The client queries the lake via **`attach_ducklake`** (preferred) or `quack_query`.

## Architecture

```text
quacktail-server                          quacktail-client
─────────────────                         ─────────────────
tailscale_up                              tailscale_up
ATTACH ducklake:… AS lake (local Parquet)  tailscale_quack_forward → quack:127.0.0.1:19494
  └─ ducklake-lake volume                   attach_ducklake → SELECT FROM lake.inventory
quack_serve(127.0.0.1:9494)               ATTACH quack:… AS remote (e2e)
tailscale_serve_local
```

Parquet + metadata live on **`ducklake-lake`** on the server only (`/var/lib/ducklake`).

## Access patterns

| Pattern | When to use |
|---------|-------------|
| **`CALL attach_ducklake(...)`** | Server owns DuckLake files — **preferred** |
| **`quack_query(uri, '…')`** | Same as above; fallback for older images |
| **`tailscale_quack_forward`** | Required on tsnet clients before Quack ATTACH |
| **`ATTACH 'ducklake:quack:…' (DATA_PATH '…')`** | Client has shared Parquet ([DuckDB 1.5.3](https://duckdb.org/2026/05/20/announcing-duckdb-153.html)) |
| **`ATTACH 'quack:…' AS remote`** | Primary catalog only — **not** `remote.lake.*` |

Full pattern guide: [docs/GUIDE.md](../docs/GUIDE.md).

## Run the demo

```bash
cd examples
docker compose build quacktail-server quacktail-client
docker compose up -d --force-recreate headscale quacktail-server
docker compose --profile test run --rm quacktail-client
```

Expect `LAKE_PASSED`, `PASSED`, and `✓ Demo passed`.

## Tailnet client SQL (sketch)

```sql
CALL tailscale_quack_forward(host => 'quacktail-server', port => 9494, local_port => 19494);

CREATE SECRET (TYPE quack, TOKEN 'quackscale-demo-token', SCOPE 'quack:127.0.0.1:19494');

CALL attach_ducklake(
    'quack:127.0.0.1:19494',
    remote_catalog => 'lake',
    alias => 'lake',
    token => 'quackscale-demo-token',
    disable_ssl => true
);
SELECT * FROM lake.inventory;

ATTACH 'quack:127.0.0.1:19494' AS remote (TYPE quack);
SELECT * FROM remote.e2e_payload;
```

See [local-demo.sql](local-demo.sql) for a standalone script.
