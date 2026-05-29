# Headscale QuackTail e2e tests

End-to-end validation for **two DuckDB nodes** on a [Headscale](https://github.com/juanfont/headscale) tailnet using the core **`quack`** extension and **`quackscale`**.

## Release binaries (required for CI)

QuackScale is **not** in the community extension repository yet. CI e2e does **not** compile from source.

1. Publish a [GitHub release](https://github.com/quackscience/duckdb-quackscale/releases) (triggers [`.github/workflows/Release.yml`](../.github/workflows/Release.yml)).
2. The workflow attaches `quacktail-linux-amd64-<tag>.tar.gz` — DuckDB **v1.5.3** with **quackscale** embedded.
3. E2e downloads that asset and runs `INSTALL quack FROM core` for the published **quack** extension.

## Manual e2e (GitHub Actions)

[`.github/workflows/headscale-e2e.yml`](../.github/workflows/headscale-e2e.yml) — **`workflow_dispatch` only**, linux.

**Actions → Headscale QuackTail e2e → Run workflow**

Optional input: **release tag** (default `latest`).

## Local run

With a published release:

```sh
eval "$(./scripts/ci_download_release_duckdb.sh v0.1.0)"   # or latest
./scripts/ci_headscale_e2e.sh
```

Or after a local build:

```sh
export DUCKDB=$PWD/build/release/duckdb
./scripts/ci_headscale_e2e.sh
```

Docker must be running. Set `QUACK_TAILNET_TOKEN` to override the default shared test token.

## What the e2e script validates

| Step | Node | Validates |
|------|------|-----------|
| Headscale Docker | control plane | Preauth key, node registration |
| Server | `quacktail-server` | `tailscale_up`, `quack_token()`, `quack_serve`, `quack_discover` |
| Client | `quacktail-client` | `tailscale_up`, shared-token `CREATE SECRET`, `quack_discover`, `ATTACH`, `INSERT`, `SELECT` |

## Related

- [docs/HEADSCALE.md](../docs/HEADSCALE.md)
- [examples/headscale_quacktail.sql](../examples/headscale_quacktail.sql)
