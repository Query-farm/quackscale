# DuckLake over QuackTail

Goal: serve DuckLake on a QuackTail node via **Quack**, reachable on the **Headscale tailnet** — **find** endpoints and **query** tables from any tailnet client.

## Status on branch `ducklake`

| Piece | Status |
|-------|--------|
| Server: local DuckLake + `quack_serve` + `tailscale_serve_local` | **Done** |
| Client: `quack_query` → `lake.inventory` (before ATTACH remote) | **Done** |
| Client `ducklake:quack:` attach (client-side `DATA_PATH`) | Documented — use when Parquet is local/shared |
| `ducklake_discover()` / enriched `quack_discover` | TBD |

## Find + query on tailnet

### 1) Find Quack servers on the tailnet

Use **`tailscale_quack_forward`** (returns `quack_uri`) or **`CALL quack_discover()`** on a node that runs quackscale locally.

Do **not** run `quack_query(..., 'FROM quack_discover()')` — it can deadlock when the server executes discover inside Quack's query handler (tsnet + quack lock contention). Remote discovery is TBD (`ducklake_discover()`).

```sql
CALL tailscale_quack_forward(host => 'quacktail-server', port => 9494, local_port => 19494);
-- → quack_uri = quack:127.0.0.1:19494
```

### 2) Query DuckLake tables

Run lake SQL **before** `ATTACH … AS remote` in the same session (mixing attached Quack catalog + extra `quack_query` calls can stall).

```sql
FROM quack_query(
    'quack:127.0.0.1:19494',
    'SELECT * FROM lake.inventory ORDER BY item_id',
    token => '…',
    disable_ssl => true
);
```

**Why not `remote.lake.inventory`?** Plain `ATTACH 'quack:…' AS remote` exposes the primary catalog only — not nested attached DuckLake catalogs.

**Why not `ducklake:quack:` in compose?** That pattern ([DuckDB 1.5.3](https://duckdb.org/2026/05/20/announcing-duckdb-153.html)) uses Quack as the catalog DB and requires client `DATA_PATH` to resolve Parquet. Our demo stores Parquet on the **server volume** only; `ducklake:quack:` attach can block when paths don't align. Use it when the client has a local or object-store `DATA_PATH` (`s3://…`).

## Architecture

```text
┌─────────────────────┐     tailnet      ┌─────────────────────┐
│  quacktail-client   │ ◄──────────────► │  quacktail-server   │
│  quack_query(…)     │                  │  ATTACH ducklake:…  │
│  (find + lake SQL)  │                  │  quack_serve        │
└─────────────────────┘                  │  ducklake-lake vol  │
                                         └─────────────────────┘
```

## Constraints

- **Quack streaming-scan limit** — one remote Quack read/write per SQL statement; see [QUACK_STREAMING.md](QUACK_STREAMING.md). Each `quack_query` call is one statement.
- **Discovery** — use `tailscale_quack_forward` / local `quack_discover()`; not `quack_query(..., quack_discover)` (deadlocks).

## Demo

[examples/ducklake/README.md](../examples/ducklake/README.md)
