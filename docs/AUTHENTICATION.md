# Authentication

QuackTail uses **two independent credential layers**. Both matter in production unless you deliberately relax Quack auth on a locked-down tailnet.

| Layer | Question | Configure with |
|-------|----------|----------------|
| **Tailnet** | Is this process on our mesh? | `TS_AUTHKEY`, Headscale preauth key, or browser login → `CALL tailscale_up` |
| **Quack** | May this caller run SQL over HTTP? | `QUACK_TAILNET_TOKEN`, `CREATE SECRET`, or custom auth macro |

Tailnet ACLs control **who can open TCP to port 9494**. Quack tokens control **who may execute SQL** once connected. See [Quack security](https://duckdb.org/docs/current/quack/security).

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

**Notes:** Production `server_url` should be HTTPS. MagicDNS is optional; `quack_uri()` prefers MagicDNS when available, else tailnet IP.

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

CALL tailscale_up(hostname => 'warehouse-a', state_dir => '…');

CALL quack_serve(
    'quack:127.0.0.1:9494',
    allow_other_hostname => true,
    token => quack_token()
);
CALL tailscale_serve_local(port => 9494);
```

**Client** (after `tailscale_quack_forward` — see [GUIDE.md](GUIDE.md)):

```sql
LOAD quack;

CREATE SECRET (
    TYPE quack,
    TOKEN 'your-shared-quack-secret',
    SCOPE 'quack:127.0.0.1:19494'
);

ATTACH 'quack:127.0.0.1:19494' AS remote (TYPE quack, DISABLE_SSL true);
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

**Each long-lived server**

1. `export TS_AUTHKEY` (or Headscale preauth key) and `export QUACK_TAILNET_TOKEN`
2. `LOAD quack; LOAD quackscale;`
3. `CALL tailscale_up(...)` with persistent `state_dir`
4. Optional: `SET GLOBAL quack_authentication_function` (Modes 2–3)
5. `CALL quack_serve(..., token => quack_token()); CALL tailscale_serve_local(port => 9494);`
6. Do **not** call `tailscale_down()` on steady-state servers

**Each one-shot client**

1. Same `QUACK_TAILNET_TOKEN` available for secrets / `quack_query`
2. `LOAD quackscale; CALL tailscale_up(...); CALL tailscale_quack_forward(...);`
3. `LOAD quack; CREATE SECRET ...;` then query / attach
4. `DETACH remote; SELECT 'done'; CALL tailscale_down();` — required or the process hangs

---

## Security

- Rotate `QUACK_TAILNET_TOKEN` like an API key; update servers and clients together
- Restrict tailnet ACLs to who may reach peer TCP **9494**
- `allow_other_hostname => true` is for tailnet binds — do not expose raw Quack on the public internet without TLS in front ([Quack exposure model](https://duckdb.org/docs/current/quack/security#exposure-model))

## References

- [Quack security](https://duckdb.org/docs/current/quack/security)
- [Quack overview — Authentication](https://duckdb.org/docs/current/quack/overview#authentication)
- [Tailscale auth keys](https://tailscale.com/kb/1085/auth-keys)
- [Headscale docs](https://headscale.net/)
