# block_chain

Discrete-event simulator for Bitcoin-style consensus under strategic mining
behavior. The project asks how realized block rewards diverge from fair-share
hashpower rewards when miners are allowed to deviate from the honest protocol.

The simulator models the incentive layer rather than cryptographic details:
blocks have simulated hashes, mining is a Poisson process, and rewards are
computed by walking the eventual canonical chain.

## Project Scope

The submission is organized into three versions:

### v0: Honest Baseline

All miners follow the honest protocol:

- each miner samples block-finding times from an exponential clock
- newly mined blocks are immediately gossiped to public peers
- nodes adopt the first-seen heaviest chain
- the leaderboard should converge toward each miner's hashpower share

This establishes that the simulator's event queue, gossip, chain selection, and
reward accounting behave correctly before attacks are added.

### v1: Selfish Mining

One aggregate selfish coalition is modeled as a single mining node with
hashpower `alpha`. It follows the Eyal-Sirer selfish-mining state machine:

- withhold private blocks while ahead
- publish one private block when the honest network starts catching up
- publish the whole private branch when doing so locks in a lead
- race honest miners when the lead falls to zero

The validation target is the classical selfish-mining result: above the
propagation-dependent threshold, the selfish coalition earns more than its fair
hashpower share. The current driver sweeps several `alpha` values and records
where the selfish score rises above `1.0`.

Multiple selfish nodes are not currently treated as one coalition. If several
nodes use `SelfishMining`, they behave as independent selfish miners, not as a
shared pool.

### v2: Topology Sweep

The final extension varies only the public peer graph while keeping the v1
selfish-mining strategy fixed. Eclipse/shadow-node attacks are intentionally out
of scope for this submission; the goal is to measure how ordinary network
structure changes selfish-mining profitability.

Planned topology families:

- `complete`: fully connected baseline
- `ring` or `line`: high-locality propagation
- `random_regular(k)`: sparse peer graph approximation
- `two_community_bridge`: clustered graph with limited cross-community links
- `hub_spoke`: centralized propagation stress test

The intended output is a sweep over topology, selfish hashpower `alpha`, and
random seed, producing a table of relative revenue and stale/orphan behavior.

There is no v3 scope. Fee markets, transaction selection, mempools, and
alternative reward schedules are left out so the final project stays focused on
selfish mining and topology.

## Running Locally

With CMake installed:

```bash
cmake -B build -G Ninja
cmake --build build
./build/run_sim
```

If Ninja is unavailable, use Makefiles instead:

```bash
cmake -B build-make -G "Unix Makefiles"
cmake --build build-make
./build-make/run_sim
```

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
