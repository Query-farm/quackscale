# Authentication

QuackScale uses **two credential layers**. On the in-process hub they can share one secret: mint a preauth key with `token` set to the same string as `QUACK_TAILNET_TOKEN`.

| Layer | Question | Configure with |
|-------|----------|----------------|
| **Mesh group** | Which peers can WireGuard to me? | `CALL quackscale_preauth(token => …)` then `authkey` on `tailscale_up` |
| **Quack** | May this caller run SQL over HTTP? | `QUACK_TAILNET_TOKEN`, `CREATE SECRET`, or custom auth macro |

The hub process is **shared**: every token group can reach it (so `ATTACH` still works). Peers that joined with different `token` values never see each other in the netmap. Untagged keys (no `token`) stay on that shared plane — do not give clients the bootstrap key if you want groups isolated.

On Tailscale SaaS / Headscale the mesh is their ACL model; Quack tokens still gate SQL once connected. See [Quack security](https://duckdb.org/docs/current/quack/security).

The default fleet uses the **in-process hub** ([below](#in-process-hub)): `quackscale_hub` on the server, `tailscale_up` on every client. Tailscale SaaS and Headscale are alternatives with the same client call.

---

## In-process hub

One DuckDB process hosts the control plane and serves SQL. Every other process joins with `tailscale_up`. No Headscale container and no Tailscale SaaS. The library behind the hub is [Wirebone](https://github.com/lmangani/wirebone.cpp); operators do not call it by that name.

| Job | SQL |
|-----|-----|
| Fleet server (hub + client) | `CALL quackscale_hub(...)` |
| Control plane only | `CALL quackscale_hub(..., join => false)` |
| Fleet client | `CALL tailscale_up(control_url => …, authkey => 'wbkey-…')` |

```sql
LOAD quackscale;

CALL quackscale_hub(
    hostname   => 'analytics-hub',
    listen     => '0.0.0.0:8080',
    server_url => 'http://10.0.0.5:8080',
    state_dir  => '/var/lib/duckdb/tailscale'
);
CALL quackscale_preauth(reusable => true, token => 'analytics');
SELECT * FROM quackscale.nodes;
SELECT * FROM quackscale.preauth_keys;
```

Clients (they do not start a hub). Repeat with a distinct `hostname` and `state_dir` per machine:

```sql
CALL tailscale_up(
    hostname    => 'analyst-1',
    control_url => 'http://10.0.0.5:8080',
    authkey     => 'wbkey-…',   -- FROM quackscale.preauth_keys for this token
    state_dir   => '/var/lib/duckdb/tailscale'
);
```

Mint one reusable key per group: `CALL quackscale_preauth(reusable => true, token => 'analytics')`. Use the same string as `QUACK_TAILNET_TOKEN`. Preauth keys and node IPs persist in the `quackscale` schema (or `backend => 'ducklake', catalog => 'lake'`). Use `backend => 'json', state_path => '…'` only for the standalone file format.

`server_url` must be an address **clients can open**. `127.0.0.1` is fine for two processes on one machine; use a LAN or overlay IP for a fleet.

`CALL quackscale_status()` reports `linked=false` if this binary was built without the hub. See [DEVELOPMENT.md](DEVELOPMENT.md). Walkthrough: [examples/wirebone](../examples/wirebone/README.md). SQL: [REFERENCE.md](REFERENCE.md#in-process-hub).

---

## Tailnet login (Tailscale SaaS)

QuackScale embeds [libtailscale](https://github.com/tailscale/libtailscale) (tsnet). Joining matches other embedded Tailscale apps.

| Mode | How | Best for |
|------|-----|----------|
| **Auth key** | `authkey` in `CALL tailscale_up`, or `TS_AUTHKEY` env | Servers, CI, automation |
| **Persisted state** | `state_dir` on disk after first login | Laptops, repeat use |
| **Browser login** | `CALL tailscale_login` → open `login_url` | First-time dev setup |

### Production server

```sh
export TS_AUTHKEY='tskey-auth-...'
```

```sql
LOAD quackscale;

CALL tailscale_up(
    hostname => 'analytics-hub',
    state_dir => '/var/lib/duckdb/tailscale'
);
```

Do not commit auth keys in SQL — use env or your secret store.

### Developer laptop

`CALL tailscale_up()` **blocks** until login completes. For a non-blocking flow:

```sql
CALL tailscale_login(
    hostname => 'my-laptop',
    state_dir => '~/.local/share/duckdb/quackscale'
);
CALL tailscale_login_status();  -- poll until status = 'up'
```

Open `login_url` in a browser. Reuse `state_dir` on later runs.

### Environment variables (tailnet)

| Variable | Effect |
|----------|--------|
| `TS_AUTHKEY` | Auth key if not passed in `CALL tailscale_up` |
| `TSNET_FORCE_LOGIN` | Force browser login even when an auth key is set (rare) |

---

## Headscale (self-hosted control plane)

[Headscale](https://github.com/juanfont/headscale) implements the Tailscale control server API. QuackScale uses the same parameters as `tailscale up --login-server`:

| Tailscale CLI | QuackScale |
|---------------|------------|
| `--login-server https://hs.example.com` | `control_url => 'https://hs.example.com'` |
| `--authkey …` | `authkey => '…'` or `TS_AUTHKEY` |
| `--hostname` | `hostname => '…'` |
| state directory | `state_dir => '…'` |

Create Headscale preauth keys with `headscale preauthkeys create` (not the Tailscale admin UI).

```sh
headscale users create quackscale
headscale preauthkeys create --user 1 --reusable --expiration 168h
```

```sql
CALL tailscale_up(
    hostname => 'duckdb-node-a',
    control_url => 'https://headscale.example.com',
    authkey => '<headscale preauth key>',
    state_dir => '/var/lib/duckdb/headscale-state'
);
```

**Compose demo:** control URL `http://headscale:8080`, preauth key written to `/work/authkey`. See [examples/README.md](../examples/README.md).

**Notes:** Production Headscale `server_url` should be HTTPS. MagicDNS is optional; `quack_uri()` prefers MagicDNS when available, else tailnet IP.

---

## Quack HTTP tokens

After a node is on the tailnet, Quack still requires application-level auth.

### Default Quack behavior (why you override it)

`CALL quack_serve(...)` generates a **random** token unless you pass `token => '...'`. That is fine for local experiments; **fleets need a shared token or allowlist**.

QuackScale provides `quack_token()` to read a shared secret from the environment on the **server**. Clients use the same value via `CREATE SECRET` or `TOKEN`.

### Environment variables (Quack)

Set on **both** servers and clients:

| Variable | Role |
|----------|------|
| `QUACK_TAILNET_TOKEN` | **Preferred** — shared token (≥ 4 characters) |
| `QUACK_TOKEN` | Fallback if `QUACK_TAILNET_TOKEN` is unset |

Keep **`TS_AUTHKEY`** separate from Quack tokens.

---

## Quack auth modes

### Mode 1 — Single shared token (recommended)

**Server:**

```sql
LOAD quack;
LOAD quackscale;

CALL quackscale_hub(hostname => 'warehouse-a', listen => '0.0.0.0:8080',
    server_url => 'http://10.0.0.5:8080', state_dir => '…');

CALL quack_serve(
    'quack:127.0.0.1:9494',
    allow_other_hostname => true,
    token => quack_token()
);
CALL tailscale_serve_local(port => 9494);
```

**Client** (same `QUACK_TAILNET_TOKEN` as the server):

```sql
LOAD quack;

CREATE SECRET (
    TYPE quack,
    TOKEN 'your-shared-quack-secret',
    SCOPE 'quack:warehouse-a.quackscale.local:9494'
);

ATTACH 'quack:warehouse-a.quackscale.local:9494' AS remote (TYPE quack, DISABLE_SSL true);
```

`SCOPE` must match how the client reaches the server. With the forwarder, that is `quack:127.0.0.1:<local_port>`.

**Stateless queries:**

```sql
FROM quack_query(
    'quack:127.0.0.1:19494',
    'SELECT 42',
    token => 'your-shared-quack-secret',
    disable_ssl => true
);
```

### Mode 2 — Token allowlist (rotation / teams)

Use Quack’s [multi-token table](https://duckdb.org/docs/current/quack/security#example-multi-token-table):

```sql
CREATE TABLE quacktail_tokens (auth_token VARCHAR PRIMARY KEY, label VARCHAR);
INSERT INTO quacktail_tokens VALUES ('primary-2026', 'analytics');

CREATE MACRO quacktail_check_token(sid, client_token, server_token) AS (
    EXISTS (SELECT 1 FROM quacktail_tokens WHERE auth_token = client_token)
);
SET GLOBAL quack_authentication_function = 'quacktail_check_token';
```

Validate **`client_token`** (what the caller sent), not `server_token`.

### Mode 3 — Developer mode (lab only)

```sql
CREATE MACRO quacktail_dev_auth(sid, client_token, server_token) AS true;
SET GLOBAL quack_authentication_function = 'quacktail_dev_auth';
```

**Not for production.** See [Quack developer mode](https://duckdb.org/docs/current/quack/security#example-developer-mode-always-allow).

---

## End-to-end checklist

**Fleet server (long-lived)**

1. `export QUACK_TAILNET_TOKEN` (and a Tailscale/Headscale key only if you are not using the hub)
2. `LOAD quack; LOAD quackscale;`
3. `CALL quackscale_hub(...)` with persistent `state_dir` (or `CALL tailscale_up` on Tailscale/Headscale)
4. Optional: `SET GLOBAL quack_authentication_function` (Modes 2–3)
5. `CALL quack_serve(..., token => quack_token()); CALL tailscale_serve_local(port => 9494);`
6. Do **not** call `tailscale_down()` or `quackscale_stop()` on a steady-state server

**Each fleet client**

1. Same `QUACK_TAILNET_TOKEN`; hub clients need a `wbkey-` minted with that `token`
2. `LOAD quackscale; CALL tailscale_up(control_url, authkey, …);`
3. `LOAD quack; CREATE SECRET ...;` then `ATTACH 'quack:analytics-hub.quackscale.local:9494'`
4. One-shot jobs: `DETACH …; CALL tailscale_down();` — required or the process hangs

---

## Security

- Rotate `QUACK_TAILNET_TOKEN` like an API key; mint a matching `quackscale_preauth(token => …)` and update servers and clients together
- One hub, many groups: different `token` values cannot WireGuard to each other; they can still reach the hub
- Do not share the hub bootstrap key with clients if you rely on group isolation
- Restrict who may reach peer TCP **9494** (mesh group + Quack token)
- `allow_other_hostname => true` is for tailnet binds — do not expose raw Quack on the public internet without TLS in front ([Quack exposure model](https://duckdb.org/docs/current/quack/security#exposure-model))

## References

- [Quack security](https://duckdb.org/docs/current/quack/security)
- [Quack overview — Authentication](https://duckdb.org/docs/current/quack/overview#authentication)
- [Tailscale auth keys](https://tailscale.com/kb/1085/auth-keys)
- [Headscale docs](https://headscale.net/)
