# QuackTail documentation

QuackTail is **DuckDB + Quack + QuackScale** on a private tailnet (Tailscale or Headscale). These docs are for **integrators** — operators wiring servers, clients, tokens, and lake catalogs — not for extension C++ development.

## Start here

| Document | Read when you need to… |
|----------|-------------------------|
| **[GUIDE.md](GUIDE.md)** | Pick a pattern, run use cases, connect clients, query DuckLake, avoid known pitfalls |
| **[AUTHENTICATION.md](AUTHENTICATION.md)** | Configure tailnet login, Headscale, and Quack HTTP tokens |
| **[../examples/README.md](../examples/README.md)** | Run the two-node Docker Compose demo |
| **[../README.md](../README.md)** | Build the extension from source and SQL command reference |

## Extension developers

| Document | Contents |
|----------|----------|
| **[DEVELOPMENT.md](DEVELOPMENT.md)** | Architecture, roadmap, updating DuckDB submodules, CI |

## Quick orientation

```text
Tailscale / Headscale     →  Is this machine on our mesh?
Quack token               →  May this caller run SQL over HTTP?
tailscale_quack_forward   →  Route Quack from embedded tsnet to 127.0.0.1
quack_serve + serve_local →  Expose DuckDB on the tailnet (:9494)
```

Load both extensions in every session:

```sql
LOAD quack;       -- HTTP server, ATTACH, quack_query
LOAD quackscale;  -- tailscale_up, forwarder, attach_ducklake, …
```

Do **not** copy the random `auth_token` from each `CALL quack_serve`. Use a **shared** fleet token — see [AUTHENTICATION.md](AUTHENTICATION.md).
