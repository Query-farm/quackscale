# Remote DuckLake attach (server-owned Parquet)

Goal: query **`lake.inventory`** on a tailnet client with normal SQL — no hand-written `quack_query(...)` — when the DuckLake catalog and Parquet files live **only on the server**.

Existing patterns stay supported; this adds an optional facade.

## Why the obvious attaches fail

| Approach | What happens |
|----------|----------------|
| `ATTACH 'quack:…' AS remote` | Primary catalog only. **`remote.lake.*` does not exist.** |
| `ATTACH 'ducklake:quack:…' AS lake (DATA_PATH '…')` | Catalog metadata over Quack; **Parquet reads use client `DATA_PATH`**. Compose demo stores files on the server volume only → hang or empty reads. |
| `quack_query(uri, 'SELECT … FROM lake.t')` | **Works** — SQL runs on server where DuckLake is attached. Verbose and easy to get wrong (quoting, session ordering). |

Reference: [DuckDB 1.5.3 DuckLake + Quack](https://duckdb.org/2026/05/20/announcing-duckdb-153.html), [usage.md](usage.md) patterns B/C.

## Design tiers

### Tier 1 — `quack_query` (today, unchanged)

```sql
FROM quack_query(
    'quack:127.0.0.1:19494',
    'SELECT * FROM lake.inventory',
    token => '…',
    disable_ssl => true
);
```

### Tier 2 — `CALL attach_ducklake(...)` (**implemented**)

One-time setup per session: discover remote tables via `quack_query`, create **local views** that delegate to the server.

```sql
LOAD quack;
LOAD quackscale;

CREATE SECRET (TYPE quack, TOKEN '…', SCOPE 'quack:127.0.0.1:19494');

CALL attach_ducklake(
    'quack:127.0.0.1:19494',
    remote_catalog => 'lake',
    alias => 'lake',
    token => '…',
    disable_ssl => true
);
-- → local_view | remote_table | status
--   lake.inventory | lake.inventory | created

SELECT * FROM lake.inventory ORDER BY item_id;
SELECT 'LAKE_PASSED' AS status, COUNT(*)::INTEGER AS inventory_rows FROM lake.inventory;
```

**How it works**

1. `quack_query` → `duckdb_tables()` on the **server** for `database_name = remote_catalog`
2. For each table: `CREATE OR REPLACE VIEW alias.table AS FROM quack_query(..., 'SELECT * FROM lake.table', ...)`
3. Client reads look like normal SQL; execution still happens on the server (server reads its Parquet)

**Limits (Tier 2)**

- No predicate/column pushdown (each scan is `SELECT *` on the server unless you add filters manually)
- Inserts/updates/deletes not supported through views (read path only for now)
- Re-run `CALL attach_ducklake` after server schema changes to refresh views
- Still obeys [Quack streaming-scan rules](QUACK_STREAMING.md) for attached `quack:` catalogs in the **same** statement; view scans use `quack_query`, not Quack attach scans

### Tier 3 — `ATTACH … TYPE quacktail_lake` (planned)

Register a **StorageExtension** in quackscale so attach syntax is native:

```sql
ATTACH 'quacktail-lake:quack:127.0.0.1:19494/lake' AS lake (
    TYPE quacktail_lake,
    TOKEN '…',
    DISABLE_SSL true
);
SELECT * FROM lake.inventory;
```

Requires a custom `Catalog` + table scan operator that issues remote reads (same transport as Tier 2, better planner integration and room for pushdown later). Does not need client `DATA_PATH`.

**Not implemented yet** — Tier 2 unblocks compose and docs without DuckDB catalog internals.

## Comparison

| | `quack_query` | Tier 2 views | `ducklake:quack:` | Tier 3 attach |
|--|---------------|--------------|-------------------|---------------|
| Server-owned Parquet | Yes | Yes | No (needs shared path) | Yes |
| SQL ergonomics | Poor | Good | Best (when paths align) | Best |
| Pushdown | N/A | No | Yes (local Parquet) | Possible |
| DML | Via wrapped SQL | Read-only | Full DuckLake | Planned |
| Changes quack/ducklake | No | No | No | No |

## Compose demo

When the built quackscale includes `attach_ducklake`, bootstrap generates:

```sql
CALL attach_ducklake('quack:127.0.0.1:19494', …);
SELECT * FROM lake.inventory …;
```

Older images fall back to `quack_query` automatically.

## Session order (unchanged)

1. `tailscale_up` → `tailscale_quack_forward`
2. `LOAD quack` + secret
3. **Lake reads** (Tier 2 or `quack_query`) **before** `ATTACH quack AS remote` if mixing with e2e attach in one session
4. `DETACH remote` + `tailscale_down` for one-shot clients

## Related

- [DUCKLAKE_TAILNET.md](DUCKLAKE_TAILNET.md) — tailnet-specific notes
- [usage.md](usage.md) — patterns A–D
- `ducklake_discover()` — planned enriched discovery (hostname + lake catalogs)
