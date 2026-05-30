# QuackTail integration guide

QuackTail combines:

1. **Tailscale or Headscale** — private mesh between nodes  
2. **Quack** — DuckDB’s HTTP protocol (`quack:` URIs, port **9494**)  
3. **QuackScale** — joins DuckDB to the tailnet and forwards Quack across it  
4. **DuckLake** (optional) — lakehouse catalog + Parquet on a QuackTail node  

QuackScale does **not** replace Quack or DuckLake. It makes them reachable on MagicDNS / `100.x.x.x` without exposing the public internet.

Credentials: [AUTHENTICATION.md](AUTHENTICATION.md). Build and SQL reference: [README.md](../README.md).

---

## Mental model

```text
┌─────────────────────────────────────────────────────────────────┐
│  quacktail-server (long-lived)                                   │
│  tailscale_up → quack_serve(127.0.0.1:9494) → tailscale_serve_local
│  optional: ATTACH ducklake:… AS lake (local or s3:// Parquet)    │
└───────────────────────────────┬─────────────────────────────────┘
                                │ tailscale_dial (encrypted)
┌───────────────────────────────▼─────────────────────────────────┐
│  quacktail-client (job, laptop, container)                       │
│  tailscale_up → tailscale_quack_forward → quack:127.0.0.1:19494   │
│  quack_query / attach_ducklake / ATTACH quack AS remote          │
│  tailscale_down() at end of one-shot sessions                      │
└─────────────────────────────────────────────────────────────────┘
```

**Why `tailscale_quack_forward`?** Quack uses normal HTTP/TCP. Embedded tsnet does not route kernel TCP to tailnet IPs. The forwarder listens on loopback and dials the peer via `tailscale_dial`.

**Why `tailscale_down`?** `tailscale_up` and the forwarder start background threads. One-shot DuckDB processes **hang after SQL finishes** unless tsnet is shut down.

---

## Choose a pattern

```text
Remote DuckDB tables (CRUD, dashboards)?
  └─► Pattern A: ATTACH 'quack:…' AS remote

Lakehouse tables (Parquet, server owns all files)?
  └─► Pattern B+: CALL attach_ducklake(...) then SELECT FROM lake.*

Lakehouse (shared Parquet — S3, NFS, identical mount on each reader)?
  └─► Pattern C: ATTACH 'ducklake:quack:…' AS lake (DATA_PATH 's3://…')

Both operational tables + lake on one node?
  └─► Pattern D: Lake queries first, then ATTACH quack AS remote (separate statements)
```

| Pattern | Client SQL | Parquet location | Best for |
|---------|------------|------------------|----------|
| **A — Quack attach** | `ATTACH 'quack:…' AS remote` | Server DuckDB / memory | Shared tables, multi-writer Quack |
| **B — quack_query** | `quack_query(uri, 'SELECT … FROM lake.t')` | Server-only | Fallback when `attach_ducklake` unavailable |
| **B+ — attach_ducklake** | `CALL attach_ducklake(...)` then `SELECT … FROM lake.t` | Server-only | **Preferred** for server-owned lakes |
| **C — ducklake:quack** | `ATTACH 'ducklake:quack:…' (DATA_PATH '…')` | Shared store / mount | Many readers with object-store access |
| **D — Hybrid** | B/B+ then A in same session | Mixed | Ops tables + lake on one tailnet node |

**Common mistake:** `SELECT * FROM remote.lake.inventory` — plain Quack attach exposes the **primary catalog only**, not nested DuckLake databases. Use B, B+, or C.

---

## Standard client connection recipe

This sequence is what the [Compose demo](../examples/README.md) proves:

```sql
LOAD quackscale;

CALL tailscale_up(
    hostname => 'my-client',
    control_url => 'http://headscale:8080',   -- omit for Tailscale SaaS
    authkey => '…',
    state_dir => '/tmp/client-tailscale',
    ephemeral => true
);

CALL tailscale_quack_forward(
    host => 'quacktail-server',
    port => 9494,
    local_port => 19494
);
CALL tailscale_ping(host => 'quacktail-server', port => 9494);  -- optional

LOAD quack;
CREATE SECRET (
    TYPE quack,
    TOKEN 'your-shared-token',
    SCOPE 'quack:127.0.0.1:19494'
);

FROM quack_query(
    'quack:127.0.0.1:19494',
    'SELECT 1 AS probe',
    token => 'your-shared-token',
    disable_ssl => true
);

-- Pattern B+, A, C, or D statements here …

DETACH remote;                              -- if Pattern A used
SELECT 'CLIENT_DEMO_DONE' AS status;        -- before tailscale_down (compose watchdog)
CALL tailscale_down();
```

---

## Use case 1 — Remote DuckDB hub (Pattern A)

**Story:** A central DuckDB node serves live tables to analysts and services on the tailnet.

### Server (long-lived)

```sql
LOAD quack;
LOAD quackscale;

CALL tailscale_up(hostname => 'analytics-hub', state_dir => '/var/lib/quacktail/hub', …);

CREATE TABLE IF NOT EXISTS events (id INTEGER, payload VARCHAR, ts TIMESTAMP);

CALL quack_serve(
    'quack:127.0.0.1:9494',
    allow_other_hostname => true,
    token => quack_token()
);
CALL tailscale_serve_local(port => 9494);

FROM quack_discover();
```

Run under systemd, Kubernetes, or the `quacktail-server` container. **Do not** call `tailscale_down()`.

### Client

```sql
LOAD quack;
LOAD quackscale;

CALL tailscale_up(…);
CALL tailscale_quack_forward(host => 'analytics-hub', port => 9494, local_port => 19494);

CREATE SECRET (TYPE quack, TOKEN '…', SCOPE 'quack:127.0.0.1:19494');

ATTACH 'quack:127.0.0.1:19494' AS remote (TYPE quack);

SELECT * FROM remote.events ORDER BY ts DESC LIMIT 10;

DETACH remote;
CALL tailscale_down();
```

---

## Use case 2 — DuckLake on the server (Patterns B / B+)

**Story:** One node holds the DuckLake catalog and Parquet; tailnet clients query without copying files.

### Server

```sql
LOAD quack;
LOAD ducklake;
LOAD quackscale;

CALL tailscale_up(hostname => 'lake-server', …);

ATTACH 'ducklake:/data/lake/metadata/warehouse.ducklake' AS lake (
    DATA_PATH '/data/lake/parquet/'
);
-- Or: DATA_PATH 's3://bucket/prefix/' with httpfs secrets on the server

CREATE TABLE IF NOT EXISTS inventory (item_id INTEGER, quantity INTEGER);
INSERT INTO inventory VALUES (101, 50);

CALL quack_serve('quack:127.0.0.1:9494', allow_other_hostname => true, token => quack_token());
CALL tailscale_serve_local(port => 9494);
```

### Client — `attach_ducklake` (preferred)

Creates local **views** that delegate to the server via `quack_query`:

```sql
LOAD quack;
LOAD quackscale;

CALL tailscale_up(…);
CALL tailscale_quack_forward(host => 'lake-server', port => 9494, local_port => 19494);

CREATE SECRET (TYPE quack, TOKEN '…', SCOPE 'quack:127.0.0.1:19494');

CALL attach_ducklake(
    'quack:127.0.0.1:19494',
    remote_catalog => 'lake',
    alias => 'lake',
    token => '…',
    disable_ssl => true
);

SELECT * FROM lake.inventory ORDER BY item_id;
```

**How it works:** discovers tables on the server with `quack_query` → `duckdb_tables()`, then `CREATE VIEW lake.t AS FROM quack_query(..., 'SELECT * FROM lake.t', ...)`.

**Limits:** read-only through views; no predicate pushdown; re-run after server schema changes.

### Client — raw `quack_query` (fallback)

```sql
FROM quack_query(
    'quack:127.0.0.1:19494',
    'SELECT * FROM lake.inventory ORDER BY item_id',
    token => '…',
    disable_ssl => true
);
```

Run lake SQL **before** `ATTACH 'quack:…' AS remote` in the same session.

**Why not `ATTACH 'ducklake:quack:…'` here?** That pattern needs client-side `DATA_PATH` to resolve Parquet. When files live only on the server volume, reads hang or return empty. Use B/B+ instead.

---

## Use case 3 — Shared Parquet (Pattern C)

**Story:** Catalog metadata flows over Quack; every reader reads Parquet from a **shared** path ([DuckDB 1.5.3 pattern](https://duckdb.org/2026/05/20/announcing-duckdb-153.html)).

### Server (catalog only)

```sql
LOAD quack;
LOAD quackscale;

CALL tailscale_up(…);
CALL quack_serve('quack:127.0.0.1:9494', allow_other_hostname => true, token => quack_token());
CALL tailscale_serve_local(port => 9494);
```

### Client

```sql
LOAD ducklake;
LOAD quack;
LOAD quackscale;

CALL tailscale_up(…);
CALL tailscale_quack_forward(host => 'lake-catalog', port => 9494, local_port => 19494);

CREATE SECRET (TYPE quack, TOKEN '…', SCOPE 'quack:127.0.0.1:19494');

ATTACH 'ducklake:quack:127.0.0.1:19494' AS warehouse (
    DATA_PATH 's3://my-bucket/lake/parquet/'
);

SELECT * FROM warehouse.inventory;
CALL tailscale_down();
```

**Requirements:** `DATA_PATH` reachable from **each client**; configure `httpfs` / cloud secrets on clients for `s3://`.

---

## Use case 4 — Hybrid hub (Pattern D)

Same tailnet node serves operational Quack tables **and** a DuckLake catalog:

1. Lake reads: `attach_ducklake` or `quack_query`  
2. Operational tables: `ATTACH 'quack:…' AS remote`  
3. **One remote Quack read/write per SQL statement** (see limitations below)  
4. End one-shot clients with `DETACH remote; CALL tailscale_down()`

---

## Finding peers

| Method | Status | Notes |
|--------|--------|-------|
| **`tailscale_quack_forward(host => '…')`** | Use today | Returns `quack_uri` for a known hostname |
| **`FROM quack_discover()`** on server | Use today | URIs this node advertises |
| **Config / DNS** | Use today | Stable Headscale hostnames in Helm, compose |
| **`quack_query(…, 'FROM quack_discover()')`** | Avoid | Can deadlock on the server |

**Fleet pattern:** deploy nodes with stable hostnames (`analytics-hub`, `lake-server`); clients call `tailscale_quack_forward(host => 'analytics-hub', …)`.

**Multiple servers:** use different local ports (`19494`, `19495`) or sequential sessions with `tailscale_down()` between peers.

**Multiple lakes on one server:** attach each with a distinct alias; clients query with fully qualified names in `quack_query` or `attach_ducklake`.

---

## Production notes

### Object storage

| Role | Approach |
|------|----------|
| Server owns lake | `DATA_PATH 's3://…'` in server `ATTACH ducklake`; clients use Pattern B+ |
| Readers with credentials | Pattern C — each client `ATTACH 'ducklake:quack:…' (DATA_PATH 's3://…')` |

### Lifecycle

| Deployment | `state_dir` | `tailscale_down` |
|------------|-------------|-------------------|
| Long-lived server | Persistent volume | Never on steady state |
| Cron / CI / compose client | Ephemeral OK | Always at end |

DuckLake metadata: file (`*.ducklake`), Postgres, or DuckDB — see [DuckLake attach](https://duckdb.org/docs/stable/duckdb/attach).

### Observability

- Server: `CALL tailscale_status()`, `/work/server.log` in compose  
- Client: `/work/client.out` in compose  
- Readiness: `CALL tailscale_ping(host => 'peer', port => 9494)` before heavy queries  

---

## Limitations and workarounds

| Issue | Workaround |
|-------|------------|
| `remote.lake.table` does not exist | Use `attach_ducklake`, `quack_query`, or `ducklake:quack:` |
| Client hangs after SQL completes | Emit done marker, then `CALL tailscale_down()` |
| Kernel TCP to `100.x:9494` fails from tsnet client | Use `tailscale_quack_forward` |
| `quack_query` + `ATTACH remote` stalls | Run lake queries **before** attach; separate statements |
| `quack_query(…, quack_discover())` hangs | Discover locally or use known hostname |

### Quack “multiple streaming scans”

This is a **core `quack` extension** limit, not QuackScale. A single SQL statement cannot perform more than one streaming read (or read + write) on the same attached Quack catalog.

Fails:

```sql
INSERT INTO remote.t SELECT 1 WHERE NOT EXISTS (SELECT 1 FROM remote.t);
```

Works (separate statements):

```sql
INSERT INTO remote.t VALUES (1, 'x') ON CONFLICT DO NOTHING;
SELECT * FROM remote.t;
```

Upstream: [duckdb/duckdb#22605](https://github.com/duckdb/duckdb/issues/22605). Split statements or use `quack_query` for one-off remote SQL.

---

## Runnable demos

| Demo | Command |
|------|---------|
| **Two-node cluster + DuckLake** | [examples/README.md](../examples/README.md) |
| **DuckLake compose details** | [examples/ducklake/README.md](../examples/ducklake/README.md) |
| **Host DuckDB → compose stack** | `scripts/local_remote_headscale_test.sh` |
| **Network-only probe** | `docker compose --profile debug run --rm tailscale-probe` |

```bash
git submodule update --init --recursive
cd examples
docker compose build quacktail-server quacktail-client
docker compose run --rm --entrypoint /usr/local/bin/quacktail-verify-image.sh quacktail-client
docker compose up -d --force-recreate headscale quacktail-server
docker compose --profile test run --rm quacktail-client
```

Expect `LAKE_PASSED`, `PASSED`, and `✓ Demo passed`.

---

## Further reading

| Resource | Topic |
|----------|--------|
| [AUTHENTICATION.md](AUTHENTICATION.md) | Tailnet + Quack credentials |
| [DEVELOPMENT.md](DEVELOPMENT.md) | Extension architecture and roadmap |
| [Quack overview](https://duckdb.org/docs/current/quack/overview) | Upstream Quack protocol |
| [DuckLake docs](https://duckdb.org/docs/stable/duckdb/ducklake) | Catalog, Parquet, attach |
