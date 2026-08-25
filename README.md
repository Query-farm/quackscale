<img width="170"  src="https://github.com/user-attachments/assets/64f4181f-9d78-486a-9afb-ea564797d9e7" />

# QuackScale

QuackScale embeds a Tailscale client ([libtailscale](https://github.com/tailscale/libtailscale)) inside DuckDB. A DuckDB process joins a private [tailnet](https://tailscale.com/docs/concepts/tailnet) and reaches its peers over encrypted WireGuard. It needs no VPN sidecar and opens no public port.

The control plane can be [Tailscale](https://tailscale.com/) SaaS, a self-hosted [Headscale](https://github.com/juanfont/headscale) process, or this DuckDB process (`CALL quackscale_hub`). Peers always join the same way: `CALL tailscale_up`.

Pair it with DuckDB's [Quack](https://duckdb.org/docs/current/quack/overview) HTTP protocol and you have **QuackTail**: SQL engines that find each other on `100.x` addresses and [MagicDNS](https://tailscale.com/docs/features/magicdns), then run `ATTACH`, `quack_query`, and DuckLake workloads across the mesh.

```sql
LOAD quack;       -- HTTP server, ATTACH, quack_query
LOAD quackscale;  -- tailscale_up to join, quackscale_hub to host
```

QuackScale is the network layer that carries DuckDB's HTTP across a tailnet, from a plain file read to `quack` and `ducklake`.

## Why QuackScale

Most teams expose a SQL engine by walling it off. You bind DuckDB or Quack to localhost, where nothing can reach it, or you bind a public IP and defend it with TLS certificates, firewall rules, and a VPN appliance. The database has no identity of its own. It trusts whatever the perimeter lets through.

QuackScale inverts that. Each DuckDB process carries its own tailnet identity and speaks WireGuard ([how Tailscale works](https://tailscale.com/blog/how-tailscale-works)) to the peers your control plane already trusts. Nothing listens on the public internet, so you have nothing there to defend.

| | Perimeter model | QuackTail |
|---|---|---|
| What listens publicly | A public IP with TLS, firewall, and VPN in front | Nothing public; Quack binds loopback and serves the mesh |
| What the database trusts | Whatever the perimeter admits | Peers its control plane already trusts |
| Encryption | TLS you configure and renew | WireGuard between every node |
| Reaching across networks | Manual port forwarding, VPN appliance | Direct paths or DERP relays |
| Extra process to run | A VPN daemon or appliance | None; tsnet (and optionally the hub) run in-process |

Each node clears two independent checks. The tailnet asks whether the machine belongs to your mesh, and the control plane decides which nodes may open a connection. A Quack token then asks whether the caller may run SQL. A stolen token buys nothing from a machine off the mesh, and a machine on the mesh still needs a token. Set `QUACK_TAILNET_TOKEN` once and the whole fleet shares one secret. See [docs/AUTHENTICATION.md](docs/AUTHENTICATION.md).

You drive all of it from SQL. Joining, status, ping, forward, serve, and teardown are `CALL` table functions, so you keep the network in the same migrations and init scripts as your data.

## Control plane

Pick one. The client path does not change.

| | Hosted Tailscale | Headscale | In-process hub |
|---|---|---|---|
| Who runs the control plane | Tailscale | A Headscale process | This DuckDB process |
| Hub / server SQL | `CALL tailscale_up(...)` | `CALL tailscale_up(control_url, authkey, …)` | `CALL quackscale_hub(...)` |
| Peer SQL | `CALL tailscale_up(...)` | Same, plus `control_url` and a Headscale key | `CALL tailscale_up(control_url, authkey, …)` |
| MagicDNS | `*.ts.net` | Your Headscale base domain | `*.quackscale.local` |
| Control-plane state | Tailscale | Headscale's store | DuckDB tables (`quackscale.*`), or DuckLake |

`quackscale_hub` starts the control plane and joins it (`join` defaults to true). `CALL quackscale_hub(..., join => false)` is control plane only. Community builds that were not linked against the hub library report `linked=false` from `quackscale_status()`; they still join Tailscale or Headscale.

## Install

QuackScale needs **DuckDB v1.5.5** and unsigned extensions. Install from the custom extension repository:

```sql
INSTALL quackscale FROM community;
LOAD quackscale;
```

From the CLI, start `duckdb -unsigned`, then `LOAD quackscale;`.

You also need `quack` (and `ducklake`, for lake workloads) from `core_nightly`:

```sql
INSTALL quack FROM core_nightly; LOAD quack;
```

Prebuilt QuackTail binary bundles ship on each [release](https://github.com/quackscience/duckdb-quackscale/releases). To build from source, see [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Quick start

### Join an existing tailnet

```sh
export TS_AUTHKEY='tskey-auth-...'              # Tailscale auth key, or a Headscale preauth key
export QUACK_TAILNET_TOKEN='your-shared-token'  # one shared secret for the whole fleet
duckdb -unsigned
```

```sql
LOAD quack;
LOAD quackscale;

CALL tailscale_up(
    hostname  => 'my-duckdb-node',
    state_dir => '~/.local/share/duckdb/quackscale'
);

CALL quack_serve('quack:127.0.0.1:9494', allow_other_hostname => true, token => quack_token());
CALL tailscale_serve_local(port => 9494);

FROM quack_discover();   -- prints this node's quack: URI on the tailnet
```

Leave a long-lived server running with a persistent `state_dir`, and do not call `tailscale_down()`. For Headscale, add `control_url` and a preauth key. See [docs/AUTHENTICATION.md](docs/AUTHENTICATION.md).

### Host the control plane here

No Tailscale account and no Headscale process. This node is the hub; peers only call `tailscale_up`. Requires a hub-linked build (`SELECT linked FROM quackscale_status()`).

```sql
LOAD quack;
LOAD quackscale;

CALL quackscale_hub(
    hostname   => 'coord',
    listen     => '0.0.0.0:8080',
    server_url => 'http://10.0.0.5:8080',
    state_dir  => '~/.local/share/duckdb/quackscale'
);

SELECT * FROM quackscale.nodes;          -- hub roster
SELECT * FROM quackscale.preauth_keys;   -- keys for peers

CALL quack_serve('quack:127.0.0.1:9494', allow_other_hostname => true, token => quack_token());
CALL tailscale_serve_local(port => 9494);
```

Create extra keys with `CALL quackscale_preauth(reusable => true)`. State lives in the `quackscale` schema of this DuckDB (or `backend => 'ducklake', catalog => 'lake'`). A two-process walkthrough is in [examples/wirebone](examples/wirebone/README.md).

### A client that reaches it

After `tailscale_up`, tailnet `quack:` addresses route over the mesh on their own. Attach the address `quack_discover()` printed on the server, with no forwarder:

```sql
LOAD quack;
LOAD quackscale;

CALL tailscale_up(
    hostname    => 'my-client',
    control_url => 'http://10.0.0.5:8080',   -- omit for Tailscale SaaS
    authkey     => 'wbkey-…',                -- or TS_AUTHKEY / Headscale key
    state_dir   => '~/.local/share/duckdb/quackscale-client'
);

CREATE SECRET (TYPE quack, TOKEN 'your-shared-token', SCOPE 'quack:100.x.x.x:9494');
ATTACH 'quack:100.x.x.x:9494' AS remote (TYPE quack, DISABLE_SSL true);
-- Hub MagicDNS: ATTACH 'quack:coord.quackscale.local:9494' …

FROM remote.query('SELECT 42');

DETACH remote;
CALL tailscale_down();   -- a one-shot client must close tsnet, or the process hangs
```

## How it fits together

```mermaid
flowchart TB
    subgraph server["Quack server"]
        s1["tailscale_up  or  quackscale_hub"]
        dl[ATTACH ducklake]
        pq[(Parquet volume)]
        sq[quack_serve + serve_local]
        s1 --> dl
        dl --- pq
        dl --> sq
    end

    wb["Hub control plane<br/>optional, same process"]
    s1 -.-> wb

    m((100.x tailnet))

    sq -->|tailscale_dial| m
    wb -.->|map / preauth| m

    c1["peer · tailscale_up + ATTACH quack"]
    c2["peer · attach_ducklake"]

    m -->|encrypted TCP| c1
    m -->|encrypted TCP| c2
```

A server joins with `tailscale_up`, or hosts the mesh itself with `quackscale_hub`. It attaches a DuckLake catalog when it owns one, and serves Quack on loopback through `quack_serve` and `tailscale_serve_local`. Clients reach it over encrypted TCP across the mesh, and no node listens on the public internet.

`tailscale_up` wraps DuckDB's HTTP layer. QuackScale then dials any tailnet host you name (`100.64.0.0/10`, `*.ts.net`, or `*.quackscale.local`) over tsnet, so `ATTACH 'quack:100.x:9494'` works on its own. Pass `http_route => false` to turn this off. Three cases fall outside the router: a bare MagicDNS short name, a pinned `127.0.0.1:<port>` endpoint, and a non-HTTP client. For those, `tailscale_quack_forward` listens on loopback and dials the peer. To read a server-owned DuckLake catalog, call `attach_ducklake`. The [guide](docs/GUIDE.md) works through each pattern.

## Where to next

| You want to… | Read |
|--------------|------|
| Pick a pattern: remote tables, server-owned DuckLake, or shared Parquet | [docs/GUIDE.md](docs/GUIDE.md) |
| Set up Tailscale, Headscale, an in-process hub, and Quack tokens | [docs/AUTHENTICATION.md](docs/AUTHENTICATION.md) |
| Look up a SQL command and its parameters | [docs/REFERENCE.md](docs/REFERENCE.md) |
| Run a two-process hub proof on one machine | [examples/wirebone](examples/wirebone/README.md) |
| Run a two-node Headscale proof on Docker Compose | [examples/README.md](examples/README.md) |
| Build the extension, or work on it | [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) |

## License

MIT, from the [DuckDB extension template](https://github.com/duckdb/extension-template). libtailscale is [BSD-3-Clause](https://github.com/tailscale/libtailscale/blob/main/LICENSE). Wirebone is [MIT](https://github.com/lmangani/wirebone.cpp).
