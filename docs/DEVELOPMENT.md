# Development

This document is for **extension contributors** — building QuackScale, updating DuckDB, and CI. Integrators should read [GUIDE.md](GUIDE.md) and [AUTHENTICATION.md](AUTHENTICATION.md).

## What QuackScale is

QuackScale is a DuckDB community extension embedding [libtailscale](https://github.com/tailscale/libtailscale) so a DuckDB process can join a tailnet and reach the [Quack](https://duckdb.org/docs/current/quack/overview) HTTP protocol on tailnet addresses.

QuackScale does **not** reimplement Quack. It provides tailnet lifecycle SQL, a localhost forwarder for Quack clients, and helpers such as `attach_ducklake`.

```text
DuckDB + quackscale + libtailscale
  → tailscale_up, tailscale_quack_forward, quack_uri, attach_ducklake
DuckDB + quackscale + wirebone (in-process hub)
  → quackscale_hub (control plane + client in one process)
DuckDB + quack (core)
  → quack_serve, ATTACH, quack_query
```

## Build

Prerequisites: C++17, cmake, ninja or make, Go 1.25+ (CGO), git submodules.

```sh
git clone --recurse-submodules https://github.com/quackscience/duckdb-quackscale.git
cd duckdb-quackscale
GEN=ninja make release
```

Artifacts:

- `build/release/duckdb`
- `build/release/extension/quackscale/quackscale.duckdb_extension`

Disable libtailscale (stub build):

```sh
make CMAKE_VARS="-DQUACKSCALE_WITH_TAILSCALE=OFF"
```

The in-process hub is **on by default** on POSIX. Sources come from `third_party/wirebone` (submodule), a sibling `../wirebone.cpp`, `QUACKSCALE_WIREBONE_DIR`, or a CMake FetchContent of [wirebone.cpp](https://github.com/lmangani/wirebone.cpp). OpenSSL (`libcrypto`), nghttp2, and libzstd are required (`vcpkg.json` supplies them in DuckDB extension CI). Hub state is stored in DuckDB tables (`quackscale.meta`, `quackscale.preauth_keys`, `quackscale.nodes`) or in an attached DuckLake catalog. Force the stub off:

```sh
make CMAKE_VARS="-DQUACKSCALE_WITH_WIREBONE=OFF"
```

Docker Compose images build from source by default — see [examples/Dockerfile](../examples/Dockerfile) and `.dockerignore`.

## Repository layout

```text
cmake/Libtailscale.cmake     Go c-archive build + Go 1.25.5 bootstrap
cmake/Wirebone.cmake          In-process coordinator (submodule, sibling, or FetchContent)
third_party/libtailscale/     git submodule
third_party/wirebone/         git submodule (lmangani/wirebone.cpp)
src/                          C++ extension (bridge, forwarder, attach_ducklake, wirebone catalog)
scripts/e2e/                  Compose entrypoint, bootstrap, verify-image
examples/                     Headscale Compose demo; examples/wirebone is local coordinator+peer
duckdb/                       DuckDB submodule
extension-ci-tools/             Extension build makefile submodule
```

## libtailscale integration

- Built with `go build -buildmode=c-archive` → `libtailscale.a`
- C API: `tailscale_up`, `tailscale_dial`, `tailscale_close`, etc.
- CMake option `QUACKSCALE_WITH_TAILSCALE` (default ON)
- Ubuntu Docker builder needs `build-essential` and `patch` for the libtailscale patch step

## Updating DuckDB

When bumping the DuckDB target:

1. Update `./duckdb` submodule to the latest stable tag  
2. Update `./extension-ci-tools` to the branch matching that DuckDB version (e.g. `v1.5.5`)  
3. Update `duckdb_version` in [MainDistributionPipeline.yml](../.github/workflows/MainDistributionPipeline.yml)  
4. Rebuild — the DuckDB C++ API is not stable; fix compile breaks using [release notes](https://github.com/duckdb/duckdb/releases) and core extension patches  

## CI workflows

| Workflow | Trigger | Purpose |
|----------|---------|---------|
| [headscale-e2e.yml](../.github/workflows/headscale-e2e.yml) | **Manual only** | Release-binary two-node e2e (no source build) |
| [headscale-integration.yml](../.github/workflows/headscale-integration.yml) | PR | Source build + Headscale smoke |
| [hub-integration.yml](../.github/workflows/hub-integration.yml) | PR | Source build + in-process hub smoke (embeds [wirebone.cpp](https://github.com/lmangani/wirebone.cpp)) |
| [Release.yml](../.github/workflows/Release.yml) | Release published / manual | Extension repo → GitHub Pages; linux QuackTail tarball → Releases |
| [libtailscale-integration.yml](../.github/workflows/libtailscale-integration.yml) | PR | libtailscale `go test` |
| [MainDistributionPipeline.yml](../.github/workflows/MainDistributionPipeline.yml) | PR | Extension distribution CI |

**Headscale e2e never runs on push/PR** and never compiles DuckDB in CI — use `workflow_dispatch` on `headscale-e2e` with a release tag. Hub integration **does** compile DuckDB (same as Headscale integration) so it can embed the hub library. Full DuckLake compose demo is local dev only (`scripts/ci_compose_e2e.sh`).

### Release and GitHub Pages

On **Release published** (or manual **Release** workflow):

1. **build** — extension-ci-tools matrix (`quackscale` per platform, same exclusions as MainDistributionPipeline).
2. **package-pages** + **deploy-pages** — unsigned `.duckdb_extension.gz` under `v1.5.5/{arch}/` at `https://quackscience.github.io/duckdb-quackscale`.
3. **build-quacktail-bundle** — linux amd64 `quacktail-linux-amd64-{tag}.tar.gz` attached to the GitHub Release.

**One-time repo setup:** Settings → Pages → Build and deployment → **Source: GitHub Actions**.

**Manual Pages-only test:** Actions → Release → Run workflow → leave `release_tag` empty, uncheck skip_pages.

**Manual tarball test:** provide an existing git tag in `release_tag`, uncheck skip_tarball.

Each Pages deploy replaces the whole site (one DuckDB version hosted). To host multiple DuckDB versions, accumulate version directories in a follow-up change.

## Roadmap (selected)

| Item | Status |
|------|--------|
| `tailscale_up`, `tailscale_quack_forward`, `tailscale_down` | Done |
| `attach_ducklake` (Tier 2 remote lake views) | Done |
| Headscale + Compose e2e | Done |
| `ATTACH … TYPE quacktail_lake` (Tier 3 native catalog) | Planned |
| `ducklake_discover()` enriched discovery | Planned |
| `quackscale_hub()` in-process control plane + client | Done (Wirebone; `-DQUACKSCALE_WITH_WIREBONE=OFF` stub) |
| Community extension descriptor publish | Done (GitHub Pages on release) |

## Risks

| Risk | Mitigation |
|------|------------|
| Large binary (Go runtime) | Document size; `QUACKSCALE_WITH_TAILSCALE=OFF` stub |
| Quack API churn | Pin DuckDB; integration tests against pinned quack |
| Secrets in SQL | Env / orchestrator secrets — see [AUTHENTICATION.md](AUTHENTICATION.md) |

## Tests

```sh
make test
```

SQL unit tests do not require a live tailnet. `test/sql/wirebone.test` expects a hub-linked build (`SELECT linked FROM quackscale_status()` is `true`). Two-process hub smoke (CI):

```sh
./scripts/ci_hub_smoke.sh
```

E2e: [test/e2e/README.md](../test/e2e/README.md), [examples/wirebone](../examples/wirebone/README.md), [examples/README.md](../examples/README.md).

## License

MIT (extension template). libtailscale is [BSD-3-Clause](https://github.com/tailscale/libtailscale/blob/main/LICENSE). Wirebone is [MIT](https://github.com/lmangani/wirebone.cpp).
