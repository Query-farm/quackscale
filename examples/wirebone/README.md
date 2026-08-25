# Two-process hub example

**Query fleet on one machine:** this DuckDB is the server (hub + Quack). A second process is a fleet client. No Tailscale account, no Headscale.

Same SQL as the README, with `127.0.0.1` so both processes share a host. A third terminal can copy `peer.sql` with a different `hostname` and `state_dir`.

**Needs:** a hub-linked build (`SELECT linked FROM quackscale_status()` is `true`). Check out [wirebone.cpp](https://github.com/lmangani/wirebone.cpp) next to this repo (or set `QUACKSCALE_WIREBONE_DIR`) and rebuild. See [docs/DEVELOPMENT.md](../../docs/DEVELOPMENT.md).

```
  fleet server                            fleet clients
  ────────────                            ─────────────
  CALL quackscale_hub                     CALL tailscale_up(
       = control plane + join               control_url, wbkey-…)
  quack_serve + serve_local          ──►  ATTACH quack:analytics-hub.quackscale.local:9494
  quackscale.nodes / preauth_keys         analyst-1, job-etl, …
```

SQL reference: [docs/REFERENCE.md](../../docs/REFERENCE.md#in-process-hub). Credentials: [docs/AUTHENTICATION.md](../../docs/AUTHENTICATION.md#in-process-hub).

## Automated mesh proof

Same script CI runs ([`scripts/ci_hub_smoke.sh`](../../scripts/ci_hub_smoke.sh)). From this directory, with `build/release/duckdb` already built at the repo root:

```bash
./run.sh
```

Expect the peer `CALL tailscale_status()` to show `running true` and a `wbkey-` printed for the join. Override the binary with `DUCKDB` or `DUCKDB_BIN`. GitHub Actions: [hub-integration.yml](../../.github/workflows/hub-integration.yml).

## Two terminals (QuackTail)

Set a shared Quack token if you will serve HTTP:

```sh
export QUACK_TAILNET_TOKEN='your-shared-token'
```

**Terminal 1 — fleet server** (`coordinator.sql`):

```bash
../../build/release/duckdb -unsigned
```

```sql
LOAD quack;
LOAD quackscale;

SELECT linked FROM quackscale_status();   -- must be true

CALL quackscale_hub(
    hostname   => 'analytics-hub',
    listen     => '127.0.0.1:18080',
    server_url => 'http://127.0.0.1:18080',
    dns_listen => '',
    state_dir  => '/tmp/quackscale-hub-coord'
);

SELECT * FROM quackscale_status();
SELECT * FROM quackscale.preauth_keys;
CALL quackscale_preauth(reusable => true);
-- copy a wbkey-… into every client session

CALL quack_serve('quack:127.0.0.1:9494', allow_other_hostname => true, token => quack_token());
CALL tailscale_serve_local(port => 9494);
FROM quack_discover();
```

Leave this process running. Do not call `tailscale_down()` or `quackscale_stop()`.

**Terminal 2 — fleet client** (`peer.sql`). A third client is the same file with `hostname => 'job-etl'` and a new `state_dir`.

```sql
LOAD quack;
LOAD quackscale;

CALL tailscale_up(
    hostname    => 'analyst-1',
    control_url => 'http://127.0.0.1:18080',
    authkey     => 'wbkey-…',
    state_dir   => '/tmp/quackscale-hub-analyst-1',
    ephemeral   => true
);

FROM tailscale_status();

CREATE SECRET (TYPE quack, TOKEN 'your-shared-token', SCOPE 'quack:analytics-hub.quackscale.local:9494');
ATTACH 'quack:analytics-hub.quackscale.local:9494' AS hub (TYPE quack, DISABLE_SSL true);

FROM hub.query('SELECT 42');

DETACH hub;
CALL tailscale_down();
```

On the server, `SELECT * FROM quackscale.nodes` should list `analytics-hub` and each client. If MagicDNS fails, attach the `100.x` from `quackscale.nodes` or `quack_discover()`, or use `tailscale_quack_forward(host => 'analytics-hub', port => 9494)`.

## Roles

| Call | This process |
|------|----------------|
| `quackscale_hub` | Control plane **and** tsnet client |
| `quackscale_hub(..., join => false)` | Control plane only |
| `tailscale_up` | Client only (every peer) |

`server_url` must be reachable from peers. `127.0.0.1` is correct for two processes on one host; use a LAN address for other machines.

## State

Default backend is DuckDB tables in the session database: `quackscale.meta`, `quackscale.preauth_keys`, `quackscale.nodes`. For a shared catalog:

```sql
CALL quackscale_hub(backend => 'ducklake', catalog => 'lake', …);
```

`backend => 'json'` is the standalone file format and is not required here.
