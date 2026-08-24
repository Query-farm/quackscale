# QuackScale SQL reference

Every function the `quackscale` extension registers. For how to combine them, see [GUIDE.md](GUIDE.md).

## Loading

```sql
LOAD quackscale;   -- this extension
LOAD quack;        -- required for quack_serve, ATTACH 'quack:…', and quack_query
```

`quack_serve`, `quack_stop`, `quack_query`, and `ATTACH 'quack:…'` belong to the upstream
[`quack` extension](https://duckdb.org/docs/current/quack/overview), not to `quackscale`. They are
documented there. `quackscale` registers the functions below.

Table functions are invoked with `CALL`. Scalar functions (`quack_uri`, `quack_token`) are invoked
in a `SELECT`. Credentials and environment variables are documented in
[AUTHENTICATION.md](AUTHENTICATION.md); they are not repeated here.

Two ports recur as defaults: `9494` is the Quack remote protocol port; `19494` is the default
loopback port for `tailscale_quack_forward`.

---

## Tailnet lifecycle

### `tailscale_up`

```sql
CALL tailscale_up(hostname => 'analytics-hub', state_dir => '/var/lib/quacktail/hub');
```

Joins the tailnet and blocks until the node is up. On success, installs the transparent HTTP router
unless `http_route => false`. Intended for servers, CI, and automation.

The first positional argument, if given, sets `hostname`.

| Parameter | Type | Default | Meaning |
|-----------|------|---------|---------|
| `hostname` | VARCHAR | none | Node name on the tailnet. May also be passed positionally. |
| `authkey` | VARCHAR | `TS_AUTHKEY` env | Tailscale or Headscale preauth key. |
| `control_url` | VARCHAR | Tailscale SaaS | Control-plane URL. Set for Headscale. |
| `state_dir` | VARCHAR | none | Directory for persisted tailnet identity. |
| `ephemeral` | BOOLEAN | `false` | Register as an ephemeral node, removed when it disconnects. |
| `loopback_proxy` | BOOLEAN | `false` | Start the libtailscale loopback SOCKS proxy (used by the deprecated `tailscale_quack_proxy`). |
| `http_route` | BOOLEAN | `true` | Install the transparent tailnet HTTP router (see [Transparent HTTP routing](#transparent-http-routing)). |

Returns one row:

| Column | Type | Meaning |
|--------|------|---------|
| `running` | BOOLEAN | Node is up. |
| `hostname` | VARCHAR | Node name, or NULL. |
| `tailnet_ips` | VARCHAR[] | Assigned tailnet addresses. |

### `tailscale_login`

```sql
CALL tailscale_login(hostname => 'my-laptop', state_dir => '~/.local/share/duckdb/quackscale');
```

Begins a non-blocking, browser-based join and returns immediately. The returned `login_url` is
opened in a browser to complete authentication; progress is polled with
[`tailscale_login_status`](#tailscale_login_status). The transparent HTTP router is installed
immediately unless `http_route => false`, and stays inert until the node comes up. Intended for
first-time interactive setup.

Accepts the same parameters as [`tailscale_up`](#tailscale_up).

Returns one row:

| Column | Type | Meaning |
|--------|------|---------|
| `status` | VARCHAR | `starting`, `needs_login`, `up`, `error`, or `idle`. |
| `login_url` | VARCHAR | Browser authentication URL, or NULL. |
| `message` | VARCHAR | Human-readable status detail. |

### `tailscale_login_status`

```sql
CALL tailscale_login_status();
```

Reports the state of an interactive login started by [`tailscale_login`](#tailscale_login). Takes no
parameters. Returns one row:

| Column | Type | Meaning |
|--------|------|---------|
| `status` | VARCHAR | `starting`, `needs_login`, `up`, `error`, or `idle`. |
| `login_url` | VARCHAR | Browser authentication URL, or NULL. |
| `message` | VARCHAR | Status detail, or NULL. |
| `running` | BOOLEAN | Node is up. |
| `hostname` | VARCHAR | Node name, or NULL. |
| `tailnet_ips` | VARCHAR[] | Assigned tailnet addresses. |

### `tailscale_status`

```sql
CALL tailscale_status();
```

Reports tailnet connectivity. Takes no parameters. Returns one row:

| Column | Type | Meaning |
|--------|------|---------|
| `libtailscale_linked` | BOOLEAN | The build links libtailscale. |
| `running` | BOOLEAN | Node is up. |
| `hostname` | VARCHAR | Node name, or NULL. |
| `tailnet_ips` | VARCHAR[] | Assigned tailnet addresses. |

### `tailscale_down`

```sql
CALL tailscale_down();
```

Stops the forwarder and closes tsnet. Takes no parameters. One-shot processes hang after their SQL
finishes unless this is called, because `tailscale_up` and the forwarder run background threads.
Returns one row:

| Column | Type | Meaning |
|--------|------|---------|
| `shutdown_ok` | BOOLEAN | Always `true`. |

---

## In-process coordinator (Wirebone)

A QuackScale build that finds [wirebone.cpp](https://github.com/lmangani/wirebone.cpp) (sibling checkout or `QUACKSCALE_WIREBONE_DIR`) can host the Tailscale/Headscale-compatible control plane **in the same process** as the tsnet client. Peers still call `tailscale_up` with `control_url` and a preauth key; they do not run Wirebone.

`CALL wirebone_status()` reports `linked=false` when the extension was built without Wirebone.

### `wirebone_serve`

```sql
CALL wirebone_serve(
    listen     => '0.0.0.0:8080',
    server_url => 'http://10.0.0.5:8080',
    domain     => 'wirebone.local'
);
SELECT * FROM wirebone.preauth_keys;
SELECT * FROM wirebone.nodes;
```

Starts the coordinator on a background thread. State lives in DuckDB tables in the `wirebone` schema of the current database (or an attached DuckLake catalog). The same process can then join as a client with [`tailscale_up`](#tailscale_up) or [`quackscale_serve`](#quackscale_serve).

| Parameter | Type | Default | Meaning |
|-----------|------|---------|---------|
| `listen` | VARCHAR | `0.0.0.0:8080` | Bind address for the control plane. |
| `server_url` | VARCHAR | `http://127.0.0.1:8080` | URL advertised to other nodes. |
| `backend` | VARCHAR | `duckdb` | `duckdb` (local tables), `ducklake` (shared catalog), or `json` (legacy file). |
| `catalog` | VARCHAR | current database | Catalog that holds schema `wirebone`. Required for `ducklake`. |
| `database` | VARCHAR | none | Dedicated `.duckdb` file when you do not want the session database. |
| `state_path` | VARCHAR | none | JSON file when `backend => 'json'`. |
| `coordinator_state` | VARCHAR | none | Alias for `state_path`. |
| `domain` | VARCHAR | `wirebone.local` | MagicDNS suffix (`hostname.domain`). |
| `dns_listen` | VARCHAR | `0.0.0.0:5353` | UDP MagicDNS listener; empty disables it. |

Tables (same schema on DuckLake): `wirebone.meta`, `wirebone.preauth_keys`, `wirebone.nodes`.

Returns one row: `bound`, `control_url`, `domain`, `preauth_key` (bootstrap key created on first serve).

### `quackscale_serve`

```sql
CALL quackscale_serve(
    hostname           => 'duckdb-coord',
    listen             => '0.0.0.0:8080',
    server_url         => 'http://10.0.0.5:8080',
    catalog            => 'memory',
    state_dir          => '/var/lib/duckdb/tailscale'
);
```

One-call coordinator **and** client: starts Wirebone, then `tailscale_up` against `http://127.0.0.1:<bound-port>` using the bootstrap preauth key. Set `join => false` to start only the control plane.

Accepts every [`wirebone_serve`](#wirebone_serve) parameter plus every [`tailscale_up`](#tailscale_up) parameter, and:

| Parameter | Type | Default | Meaning |
|-----------|------|---------|---------|
| `join` | BOOLEAN | `true` | Also join the mesh as a tsnet client. |

Returns one row: `bound`, `control_url`, `domain`, `preauth_key`, `running`, `hostname`, `tailnet_ips`.

### `wirebone_status`

```sql
CALL wirebone_status();
```

| Column | Type | Meaning |
|--------|------|---------|
| `linked` | BOOLEAN | This build embeds Wirebone. |
| `running` | BOOLEAN | Coordinator thread is up. |
| `bound` | VARCHAR | Actual listen address, or NULL. |
| `control_url` | VARCHAR | Advertised URL, or NULL. |
| `domain` | VARCHAR | MagicDNS domain, or NULL. |
| `preauth_key` | VARCHAR | Bootstrap key, or NULL. |

### `wirebone_preauth`

```sql
CALL wirebone_preauth(reusable => true);
```

Creates an additional preauth key. Requires a running coordinator.

| Parameter | Type | Default | Meaning |
|-----------|------|---------|---------|
| `reusable` | BOOLEAN | `true` | Key may be used more than once. |
| `ephemeral` | BOOLEAN | `false` | Nodes registered with this key are ephemeral. |

Returns `key`, `reusable`, `ephemeral`.

### `wirebone_bootstrap_key`

```sql
SELECT wirebone_bootstrap_key();
```

Scalar. Returns the bootstrap preauth key from the running coordinator.

### `wirebone_nodes`

```sql
CALL wirebone_nodes();
```

Registered nodes. Empty when the coordinator is not running.

| Column | Type | Meaning |
|--------|------|---------|
| `id` | UBIGINT | Numeric node id. |
| `hostname` | VARCHAR | Node name. |
| `ipv4` | VARCHAR | Allocated `100.64/10` address. |
| `ipv6` | VARCHAR | Allocated ULA. |
| `node_key` | VARCHAR | `nodekey:…` |
| `online` | BOOLEAN | Recently seen on `/machine/map`. |

### `wirebone_stop`

```sql
CALL wirebone_stop();
```

Stops the coordinator thread. Does not call `tailscale_down`. Returns `stopped=true`.

---

## Connectivity on the mesh

### `tailscale_serve_local`

```sql
CALL tailscale_serve_local(port => 9494);
```

Configures Tailscale Serve to forward tailnet TCP on `port` to `127.0.0.1:local_port`. Run on a
server after `quack_serve` so peers can reach the local Quack listener.

| Parameter | Type | Default | Meaning |
|-----------|------|---------|---------|
| `port` | BIGINT | `9494` | Tailnet-facing port. Must be 1–65535. |
| `local_port` | BIGINT | value of `port` | Loopback port to forward to. Must be 1–65535. |

Returns one row:

| Column | Type | Meaning |
|--------|------|---------|
| `listen_port` | INTEGER | Tailnet-facing port. |
| `local_port` | INTEGER | Loopback target port. |
| `local_forward` | VARCHAR | `127.0.0.1:<local_port>`. |

### `tailscale_ping`

```sql
CALL tailscale_ping(host => 'peer', port => 9494);
```

Dials `host:port` over tsnet to confirm a peer is reachable before an `ATTACH` or query. Requires
the node to be up. Errors if the dial fails.

| Parameter | Type | Default | Meaning |
|-----------|------|---------|---------|
| `host` | VARCHAR | required | Tailnet host to dial. |
| `port` | BIGINT | `9494` | Port to dial. Must be 1–65535. |
| `timeout_ms` | BIGINT | `5000` | Dial timeout in milliseconds. Must be positive. |

Returns one row:

| Column | Type | Meaning |
|--------|------|---------|
| `host` | VARCHAR | Host dialed. |
| `port` | INTEGER | Port dialed. |
| `reachable` | BOOLEAN | `true` on a successful dial. |

### `tailscale_quack_forward`

```sql
CALL tailscale_quack_forward(host => 'peer', port => 9494, local_port => 19494);
```

Opens a loopback listener that dials `host:port` over tsnet for each incoming Quack connection, and
returns a `quack:127.0.0.1:<local_port>` URI. Used for MagicDNS short names (which the transparent
router does not match), a pinned local port, or non-HTTP clients; otherwise
`ATTACH 'quack:100.x:9494'` works directly after `tailscale_up`. Requires the node to be up.

| Parameter | Type | Default | Meaning |
|-----------|------|---------|---------|
| `host` | VARCHAR | required | Tailnet peer to dial. |
| `port` | BIGINT | `9494` | Remote port. Must be 1–65535. |
| `local_port` | BIGINT | `19494` | Loopback listen port. Must be 0–65535; `0` lets the OS choose. |

Returns one row:

| Column | Type | Meaning |
|--------|------|---------|
| `active` | BOOLEAN | Listener is running. |
| `remote_host` | VARCHAR | Peer host, or NULL. |
| `remote_port` | INTEGER | Peer port. |
| `local_port` | INTEGER | Loopback listen port. |
| `quack_uri` | VARCHAR | `quack:127.0.0.1:<local_port>`, or NULL. |

### Transparent HTTP routing

When `tailscale_up` or `tailscale_login` runs with `http_route => true` (the default), QuackScale
installs a global HTTP util that intercepts `http://` requests to tailnet hosts and dials them over
tsnet. A tailnet host is an IPv4 address in `100.64.0.0/10` or a `*.ts.net` MagicDNS name. Bare
MagicDNS short names are not matched and still require `tailscale_quack_forward`. All other HTTP
traffic, and all `https://`, passes to the underlying util unchanged. With routing on,
`ATTACH 'quack:100.x:9494'` and `ATTACH 'quack:host.ts.net:9494'` work without a forwarder.

---

## Quack helpers

These describe how the local node appears as a Quack endpoint on the tailnet. `quack_serve` and
`ATTACH` themselves come from the `quack` extension and require `LOAD quack`.

### `quack_uri`

```sql
SELECT quack_uri();
```

Scalar function. Returns this node's client-facing `quack:<host>:9494` URI, preferring MagicDNS and
falling back to the tailnet IP. Takes no arguments. Errors if the node is not up.

### `quack_token`

```sql
SELECT quack_token();
```

Scalar function. Returns the shared Quack token read from the `QUACK_TAILNET_TOKEN` environment
variable, or `QUACK_TOKEN` if the first is unset. Takes no arguments. Errors if neither is set or the
token is shorter than four characters. See [AUTHENTICATION.md](AUTHENTICATION.md).

### `quack_discover`

```sql
CALL quack_discover(port => 9494);
```

Lists every `quack:` URI this node advertises on the tailnet, one row per MagicDNS name and per
tailnet IP.

| Parameter | Type | Default | Meaning |
|-----------|------|---------|---------|
| `port` | BIGINT | `9494` | Advertised port. Must be 1–65535. |

Returns one row per endpoint:

| Column | Type | Meaning |
|--------|------|---------|
| `listen_uri` | VARCHAR | Full `quack:<host>:<port>` URI. |
| `host` | VARCHAR | MagicDNS name or tailnet IP. |
| `port` | INTEGER | Advertised port. |
| `via` | VARCHAR | `magicdns` or `tailnet_ip`. |

---

## Remote DuckLake

### `attach_ducklake`

```sql
CALL attach_ducklake(
    'quack:127.0.0.1:19494',
    remote_catalog => 'lake',
    alias => 'lake',
    token => '…',
    disable_ssl => true
);
```

Creates a local schema of views over the tables of a DuckLake catalog attached on a remote Quack
server, when the Parquet files live only on that server. Each view delegates to the server through
`quack_query`. Requires `LOAD quack`. The views are read-only, do not push down predicates, and must
be re-created after the server schema changes. The first positional argument is the Quack URI of the
server.

| Parameter | Type | Default | Meaning |
|-----------|------|---------|---------|
| *(positional)* | VARCHAR | required | Quack URI of the remote server. |
| `remote_catalog` | VARCHAR | `'lake'` | Database name of the DuckLake catalog on the server. |
| `alias` | VARCHAR | value of `remote_catalog` | Local schema name for the created views. |
| `token` | VARCHAR | none | Quack token forwarded to the server. |
| `disable_ssl` | BOOLEAN | `true` | Connect over plaintext HTTP (the tailnet is the encryption layer). |

`remote_catalog` and `alias` must be valid SQL identifiers (`[A-Za-z_][A-Za-z0-9_]*`). Errors if the
remote catalog holds no tables.

Returns one row per created view:

| Column | Type | Meaning |
|--------|------|---------|
| `local_view` | VARCHAR | `<alias>.<table>`. |
| `remote_table` | VARCHAR | `<remote_catalog>.<table>`. |
| `status` | VARCHAR | Always `created`. |

---

## (Deprecated) legacy SOCKS proxy

These predate transparent HTTP routing and `tailscale_quack_forward`. New deployments use the
forwarder.

### `tailscale_quack_proxy`

```sql
CALL tailscale_quack_proxy();
```

Deprecated. Enables a libtailscale loopback SOCKS proxy and exports `ALL_PROXY` so Quack HTTP routes
through tsnet. Takes no parameters. Requires the node to be up. Use
[`tailscale_quack_forward`](#tailscale_quack_forward) instead. Returns one row:

| Column | Type | Meaning |
|--------|------|---------|
| `active` | BOOLEAN | Proxy is running. |
| `listen_addr` | VARCHAR | Loopback listen address, or NULL. |
| `proxy_url` | VARCHAR | SOCKS URL with the password redacted, or NULL. |

### `tailscale_proxy_status`

```sql
CALL tailscale_proxy_status();
```

Deprecated. Reports the state of the legacy SOCKS proxy. Takes no parameters. Returns one row:

| Column | Type | Meaning |
|--------|------|---------|
| `enabled` | BOOLEAN | Proxy was requested. |
| `active` | BOOLEAN | Proxy is running. |
| `listen_addr` | VARCHAR | Loopback listen address, or NULL. |
| `proxy_url` | VARCHAR | SOCKS URL with the password redacted, or NULL. |

---

## See also

| Resource | Topic |
|----------|-------|
| [GUIDE.md](GUIDE.md) | How to combine these functions into working deployments. |
| [AUTHENTICATION.md](AUTHENTICATION.md) | Tailnet and Quack credentials, environment variables. |
| [Why QuackScale](../README.md#why-quackscale) | Rationale and design. |
| [Quack overview](https://duckdb.org/docs/current/quack/overview) | The upstream `quack` extension: `quack_serve`, `ATTACH 'quack:…'`, `quack_query`. |
| [DuckLake docs](https://duckdb.org/docs/stable/duckdb/ducklake) | Catalog, Parquet, and attach. |
