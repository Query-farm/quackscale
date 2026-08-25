# QuackTail documentation

QuackTail is **DuckDB + Quack + QuackScale** as a query fleet on a private mesh (in-process hub by default; Tailscale or Headscale if you already have them). These docs are for **integrators** — operators wiring servers, clients, tokens, and lake catalogs — not for extension C++ development.

## Start here

| Document | Read when you need to… |
|----------|-------------------------|
| **[Why QuackScale](../README.md#why-quackscale)** | Understand why a DuckDB process carries its own tailnet identity, and how the pieces fit |
| **[GUIDE.md](GUIDE.md)** | Pick a pattern, run use cases, connect clients, query DuckLake, avoid known pitfalls |
| **[AUTHENTICATION.md](AUTHENTICATION.md)** | Configure Tailscale, Headscale, an in-process hub, and Quack HTTP tokens |
| **[REFERENCE.md](REFERENCE.md)** | Look up a `quackscale` SQL command and its parameters |
| **[../examples/wirebone/README.md](../examples/wirebone/README.md)** | Run a fleet server + client on one machine (in-process hub) |
| **[../examples/README.md](../examples/README.md)** | Run the Headscale Docker Compose demo |

## Extension developers

| Document | Contents |
|----------|----------|
| **[DEVELOPMENT.md](DEVELOPMENT.md)** | Architecture, roadmap, build from source, updating DuckDB submodules, CI |

## Quick orientation

```text
quackscale_hub                    →  This process is the fleet server (control plane + join)
tailscale_up                      →  This process is a fleet client
Quack token                       →  May this caller run SQL over HTTP?
tailscale_quack_forward           →  Route Quack from embedded tsnet to 127.0.0.1
quack_serve + serve_local         →  Expose DuckDB on the mesh (:9494)
```

Load both extensions in every session:

```sql
LOAD quack;       -- HTTP server, ATTACH, quack_query
LOAD quackscale;  -- tailscale_up, quackscale_hub, forwarder, attach_ducklake, …
```

Do **not** copy the random `auth_token` from each `CALL quack_serve`. Use a **shared** fleet token — see [AUTHENTICATION.md](AUTHENTICATION.md).
