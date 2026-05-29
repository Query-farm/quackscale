# Quack authentication on a tailnet (QuackTail)

This document covers **only Quack** — HTTP application tokens after your node is already on the tailnet.

For **Tailscale node login** (`TS_AUTHKEY`, browser URL, `state_dir`), see **[AUTHENTICATION.md](AUTHENTICATION.md)**.

| Doc | Topic |
|-----|--------|
| [AUTHENTICATION.md](AUTHENTICATION.md) | Tailscale — `tailscale_up`, `TS_AUTHKEY`, browser login |
| [PLAN.md](PLAN.md) | Architecture and roadmap |
| [../README.md](../README.md) | End-to-end quick start |

---

## Goal: semi-automatic QuackTail peers

You want DuckDB servers and clients on the **same tailnet** to:

1. Find each other via `quack:hostname:9494` (QuackScale + MagicDNS).
2. Authenticate to Quack **without** copying a new random `auth_token` from every `CALL quack_serve`.

That is supported today using Quack’s built-in **`token =>`** parameter and **[Overriding authentication](https://duckdb.org/docs/current/quack/security#overriding-authentication)** — no QuackScale changes to Quack’s wire protocol.

## Two layers (do not merge them)

| Layer | Proves | You configure |
|-------|--------|----------------|
| **Tailscale** | Machine is on the tailnet | `TS_AUTHKEY`, `CALL tailscale_up`, ACLs — [AUTHENTICATION.md](AUTHENTICATION.md) |
| **Quack** | Caller may use this DuckDB session over HTTP | `QUACK_TAILNET_TOKEN`, `CREATE SECRET`, or custom `quack_authentication_function` |

A host on your tailnet is **not** automatically trusted for SQL. Tailscale is necessary but not sufficient.

## What Quack does by default (and why you override it)

From [Quack security — Default configuration](https://duckdb.org/docs/current/quack/security#default-configuration):

1. `CALL quack_serve(...)` **generates a random token** and returns it in the `auth_token` column — unless you pass `token => '...'`.
2. The default hook `quack_check_token` requires **client token == server token** for that listener.

That default is fine for a single local experiment. For a **fleet** of QuackTail nodes, you want:

- The **same** token on every server and client (env / secret manager), **or**
- A **shared allowlist** of valid tokens (SQL table + custom macro).

QuackScale’s `quack_token()` only helps read a shared token from the environment on the **server** side. Clients still use `CREATE SECRET` or `TOKEN` with the same value.

## Environment variables (Quack layer)

Set on **both** servers and clients (container env, systemd, K8s `Secret`, etc.):

| Variable | Role |
|----------|------|
| `QUACK_TAILNET_TOKEN` | **Preferred** — shared Quack auth token (≥ 4 characters) |
| `QUACK_TOKEN` | Fallback alias if `QUACK_TAILNET_TOKEN` is unset |

Keep **`TS_AUTHKEY`** separate — it is Tailscale-only ([AUTHENTICATION.md](AUTHENTICATION.md)).

Example provisioning:

```sh
export TS_AUTHKEY='tskey-auth-...'
export QUACK_TAILNET_TOKEN='your-shared-quack-secret'
duckdb
```

---

## Mode 1 — Single shared token (recommended for QuackTail)

One secret, same everywhere. Matches default `quack_check_token` when server and client use the same string.

### Server

```sql
LOAD quack;
LOAD quackscale;

CALL tailscale_up(
    hostname => 'warehouse-a',
    state_dir => '/var/lib/duckdb/tailscale'
);

CALL quack_serve(
    quack_uri(),
    allow_other_hostname => true,
    token => quack_token()    -- reads QUACK_TAILNET_TOKEN / QUACK_TOKEN
);

CALL quack_discover();
```

`quack_token()` fails fast if the env var is missing or shorter than 4 characters (Quack’s minimum).

Without the helper, pass the literal explicitly:

```sql
CALL quack_serve(quack_uri(), allow_other_hostname => true, token => 'same-secret-as-clients');
```

### Client — semi-automatic with `CREATE SECRET`

Create the secret once per client process (inject the token from your shell/env when launching DuckDB — do not hardcode in shared SQL files):

```sql
LOAD quack;

CREATE SECRET (
    TYPE quack,
    TOKEN 'your-shared-quack-secret',
    SCOPE 'quack:warehouse-a:9494'
);

ATTACH 'quack:warehouse-a:9494' AS warehouse (
    TYPE quack,
    DISABLE_SSL true
);

FROM warehouse.query('SELECT 42');
```

- **`SCOPE`** must match how clients reach the server (`quack:<hostname>:9494`). Use the hostname from `CALL tailscale_up(hostname => 'warehouse-a')`.
- After the secret exists, `ATTACH` needs no `TOKEN` clause — Quack picks it up automatically ([overview — Authentication](https://duckdb.org/docs/current/quack/overview#authentication)).

### Client — explicit token per attach

```sql
ATTACH 'quack:warehouse-a:9494' AS warehouse (
    TYPE quack,
    TOKEN 'your-shared-quack-secret',
    DISABLE_SSL true
);
```

### Client — stateless `quack_query`

```sql
FROM quack_query(
    'quack:warehouse-a:9494',
    'SELECT 42',
    token => 'your-shared-quack-secret',
    disable_ssl => true
);
```

---

## Mode 2 — Shared allowlist (multiple tokens / rotation)

When several tokens should work across the fleet (teams, rotation, read-only clients), use Quack’s **[multi-token table](https://duckdb.org/docs/current/quack/security#example-multi-token-table)** pattern.

Run once per DuckDB database (or in your bootstrap migration):

```sql
CREATE TABLE quacktail_tokens (
    auth_token VARCHAR PRIMARY KEY,
    label VARCHAR
);

INSERT INTO quacktail_tokens VALUES
    ('primary-team-token-2026', 'analytics'),
    ('readonly-team-token-2026', 'readonly');

CREATE MACRO quacktail_check_token(sid, client_token, server_token) AS (
    EXISTS (SELECT 1 FROM quacktail_tokens WHERE auth_token = client_token)
);

SET GLOBAL quack_authentication_function = 'quacktail_check_token';
```

Important behavior ([Overriding authentication](https://duckdb.org/docs/current/quack/security#overriding-authentication)):

| Argument | Meaning |
|----------|---------|
| `client_token` | What the client sent (`TOKEN`, secret, or `quack_query`) — **validate this** |
| `server_token` | From `quack_serve(token => ...)` — you may **ignore** it when using a table |

Any token listed in `quacktail_tokens` is accepted on **every** server that uses this macro (unless you add per-server logic).

Start the server with a token that satisfies Quack’s length check (still pass `token =>`):

```sql
CALL quack_serve(quack_uri(), allow_other_hostname => true, token => quack_token());
```

Clients use **their** token from the allowlist (via `CREATE SECRET` or `TOKEN`).

Populate `quacktail_tokens` from your deployment tool at startup (INSERT from env). The auth callback runs in a **fresh transient connection** — it cannot read your shell env by itself.

---

## Mode 3 — Developer mode (tailnet is the only gate)

Only on isolated lab tailnets. Admits **all** Quack clients with no token check:

```sql
CREATE MACRO quacktail_dev_auth(sid, client_token, server_token) AS true;
SET GLOBAL quack_authentication_function = 'quacktail_dev_auth';
```

See [developer mode](https://duckdb.org/docs/current/quack/security#example-developer-mode-always-allow). **Not for production.**

---

## End-to-end checklist

**On each server**

1. `export TS_AUTHKEY` and `export QUACK_TAILNET_TOKEN`
2. `LOAD quack; LOAD quackscale;`
3. `CALL tailscale_up(hostname => '...', state_dir => '...');`
4. (Optional) `SET GLOBAL quack_authentication_function` if using Mode 2 or 3
5. `CALL quack_serve(quack_uri(), allow_other_hostname => true, token => quack_token());`

**On each client**

1. Same `QUACK_TAILNET_TOKEN` available when creating secrets or attaching
2. `LOAD quack;`
3. `CREATE SECRET (TYPE quack, TOKEN '...', SCOPE 'quack:<hostname>:9494');`
4. `ATTACH 'quack:<hostname>:9494' AS ... (TYPE quack, DISABLE_SSL true);`

**Network**

- Tailscale ACL: allow tagged nodes → TCP 9494 on peers
- Quack default port: **9494** ([overview](https://duckdb.org/docs/current/quack/overview))

---

## Comparison: default vs QuackTail

| | Default Quack | QuackTail (Mode 1) |
|---|---------------|---------------------|
| Server token | Random per `quack_serve` | Fixed via `QUACK_TAILNET_TOKEN` / `quack_token()` |
| Client setup | Copy `auth_token` from server output | Same env secret or `CREATE SECRET` |
| Discovery | Manual URI | `CALL quack_discover()`, `quack_uri()` |
| Transport | Often localhost | Tailscale tailnet + `allow_other_hostname => true` |

---

## What QuackScale does not do (yet)

- Does not call `quack_serve` or install auth macros automatically — compose SQL after `CALL tailscale_up`
- Does not sync tokens over Tailscale — use env, K8s, Vault, etc.
- Planned: `quacktail_serve()` helper chaining tailnet up + shared token + `quack_serve`

---

## Security notes

- Rotate `QUACK_TAILNET_TOKEN` like an API key; update servers, client secrets, and `quacktail_tokens` together
- Use [Tailscale ACLs](https://tailscale.com/kb/1018/acls) to limit who reaches port 9494
- `allow_other_hostname => true` is for tailnet binds — do not expose raw Quack to the public internet without a TLS reverse proxy ([Quack security — Exposure model](https://duckdb.org/docs/current/quack/security#exposure-model))

## References

- [Quack security](https://duckdb.org/docs/current/quack/security) — overriding authentication & authorization
- [Quack overview — Authentication](https://duckdb.org/docs/current/quack/overview#authentication)
- [DuckDB secrets manager](https://duckdb.org/docs/current/configuration/secrets_manager)
- [Tailscale authentication (QuackScale)](AUTHENTICATION.md)
