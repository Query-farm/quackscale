# Two-process hub example

One DuckDB process is the mesh hub. A second process joins as a client. No Tailscale account, no Headscale container.

**Needs:** a hub-linked QuackScale build (`SELECT linked FROM quackscale_status()` is `true`). Check out [wirebone.cpp](https://github.com/lmangani/wirebone.cpp) next to this repo (or set `QUACKSCALE_WIREBONE_DIR`) and rebuild. See [docs/DEVELOPMENT.md](../../docs/DEVELOPMENT.md).

```
  hub (this machine)                      peer
  ──────────────────                      ────
  CALL quackscale_hub                     CALL tailscale_up(
       = control plane + tailscale_up       control_url, wbkey-…)
  quackscale.nodes / preauth_keys
  optional: quack_serve + serve_local ──► ATTACH quack:coord.quackscale.local:9494
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

**Terminal 1 — hub** (`coordinator.sql`):

```bash
../../build/release/duckdb -unsigned
```

```sql
LOAD quack;
LOAD quackscale;

SELECT linked FROM quackscale_status();   -- must be true

CALL quackscale_hub(
    hostname   => 'coord',
    listen     => '127.0.0.1:18080',
    server_url => 'http://127.0.0.1:18080',
    dns_listen => '',
    state_dir  => '/tmp/quackscale-hub-coord'
);

SELECT * FROM quackscale_status();
SELECT * FROM quackscale.preauth_keys;
-- copy the wbkey-… into the peer session

CALL quack_serve('quack:127.0.0.1:9494', allow_other_hostname => true, token => quack_token());
CALL tailscale_serve_local(port => 9494);
FROM quack_discover();
```

Leave this process running. Do not call `tailscale_down()` or `quackscale_stop()`.

**Terminal 2 — peer** (`peer.sql`):

```sql
LOAD quack;
LOAD quackscale;

CALL tailscale_up(
    hostname    => 'peer',
    control_url => 'http://127.0.0.1:18080',
    authkey     => 'wbkey-…',
    state_dir   => '/tmp/quackscale-hub-peer',
    ephemeral   => true
);

FROM tailscale_status();

CREATE SECRET (TYPE quack, TOKEN 'your-shared-token', SCOPE 'quack:coord.quackscale.local:9494');
ATTACH 'quack:coord.quackscale.local:9494' AS remote (TYPE quack, DISABLE_SSL true);

FROM remote.query('SELECT 42');

DETACH remote;
CALL tailscale_down();
```

On the hub, `SELECT * FROM quackscale.nodes` should list both hostnames. If MagicDNS fails, attach the peer to the hub's `100.x` from `quackscale.nodes` or `quack_discover()`, or use `tailscale_quack_forward(host => 'coord', port => 9494)`.

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
