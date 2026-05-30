# Testing

## Unit tests (SQLLogic)

```bash
make test
```

SQL tests under [`sql/`](sql/) do not require a live tailnet or `QUACK_TAILNET_TOKEN`.

## Integration tests (Headscale + QuackTail)

| Layer | Location |
|-------|----------|
| **Docker Compose demo** | [`examples/README.md`](../examples/README.md) |
| **E2e details** | [`e2e/README.md`](e2e/README.md) |
| **CI workflow** | [`.github/workflows/headscale-e2e.yml`](../.github/workflows/headscale-e2e.yml) |
| **libtailscale smoke** | [`.github/workflows/libtailscale-integration.yml`](../.github/workflows/libtailscale-integration.yml) |
| **Headscale smoke** | [`.github/workflows/headscale-integration.yml`](../.github/workflows/headscale-integration.yml) |

Release binaries (linux amd64, quackscale embedded) are built by [`.github/workflows/Release.yml`](../.github/workflows/Release.yml) on GitHub **Release published**.
