# block_chain

## Docker

Three lifecycles, three update cadences:

1. **Image** (toolchain) — rebuilt when `Dockerfile.dev` changes. Rare.
2. **Build configuration** — re-run cmake when `CMakeLists.txt` changes. Occasional.
3. **Compiled binaries** — rebuilt every code edit. Constant.

The bind mount in `compose.yaml` makes (3) require nothing more than invoking the compiler inside the container.

### Run the published image

```bash
docker pull ghcr.io/bcupps9/chain_image:latest
docker run --rm ghcr.io/bcupps9/chain_image:latest
```

### Local dev workflow

```bash
docker compose build dev                              # one-time (or after Dockerfile.dev edits)
docker compose run --rm dev cmake -B build -G Ninja   # when CMakeLists.txt changes
docker compose run --rm dev cmake --build build       # every code change
docker compose run --rm dev ./build/run_sim           # run the binary
docker compose run --rm dev bash                      # interactive shell
```

Suggested alias:

```bash
alias dr='docker compose run --rm dev'
```

On macOS, `UID` is not exported by default, so `compose.yaml` falls back to `1000:1000`. To match your host user (recommended so build artifacts aren't owned by root/1000), `export UID` in your shell, or hardcode the values in `compose.yaml`.

### Publishing to GHCR

```bash
docker build -t ghcr.io/bcupps9/chain_image:latest .
docker push ghcr.io/bcupps9/chain_image:latest
```

For version pinning, tag both `:latest` and `:v0.X` and push both.

### Two Dockerfiles, two purposes

- `Dockerfile.dev` — toolchain only. Local-only; never pushed (useless without bind-mounted source).
- `Dockerfile` — bakes source in. This is what gets pushed to GHCR for distribution.
