# chunky_mcu_updater. `just` lists the recipes.

default:
    @just --list

# All tests: unit tests, planner properties, security cases and the power-cut sweeps (CHUNKY_SWEEP_SEEDS=500 digs deeper)
test:
    cargo test --workspace

# clippy (warnings are errors), formatting, and the firmware-target builds of both trust options
lint:
    cargo clippy --workspace --all-targets -- -D warnings
    cargo fmt --check
    just check-fw
    just bench-lint

# The no_std crates build for Cortex-M0+ and M4F, signed and unsigned, with a series-* feature or chip-custom
check-fw:
    cargo check -q -p chunky_swap --target thumbv6m-none-eabi --features ed25519,chunky/series-stm32g0
    cargo check -q -p chunky_swap --target thumbv6m-none-eabi --features unsigned,chunky/chip-custom
    cargo check -q -p chunky_swap --target thumbv7em-none-eabihf --features ed25519,chunky/series-stm32g4
    cargo check -q -p chunky_swap --target thumbv7em-none-eabihf --features unsigned,chunky/chip-custom
    cargo check -q -p chunky_bank --target thumbv6m-none-eabi --features ed25519,chunky/chip-custom
    cargo check -q -p chunky_bank --target thumbv7em-none-eabihf --features ed25519,chunky/series-stm32h7
    cargo check -q -p chunky_bank --target thumbv7em-none-eabihf --features unsigned,chunky/series-stm32h7

# Build everything in release mode and install the `chunky` binary (cargo bin dir)
install:
    cargo build --release --workspace --locked
    cargo install --path chunky_cli --locked

# Bring this PC up to main: pull (refuses on local changes), then install
deploy:
    git pull --ff-only
    just install

# The power-cut bench harness (bench/host, own workspace; the glitch git dependency is fetched by git over ssh)
bench-cargo := "CARGO_NET_GIT_FETCH_WITH_CLI=true cargo"

# Tests of the bench harness: run loop, sweep and CSV on the simulated bench (no hardware)
bench-test:
    {{bench-cargo}} test --manifest-path bench/host/Cargo.toml

# clippy (warnings are errors) and formatting of the bench harness
bench-lint:
    {{bench-cargo}} clippy --manifest-path bench/host/Cargo.toml --all-targets -- -D warnings
    {{bench-cargo}} fmt --all --manifest-path bench/host/Cargo.toml --check

# Measure mode on the bench (bench/README.md M1-M3), e.g. `just bench-measure --pre-trigger-us 30000 --cut 0`
bench-measure *ARGS:
    {{bench-cargo}} run -q --release --manifest-path bench/host/Cargo.toml -- measure {{ARGS}}

# A sweep into bench/results/NAME, e.g. `just bench-sweep --name s1 --from 3ms --to 30ms --pre-trigger-us 12000` (--dry-run: simulated)
bench-sweep *ARGS:
    {{bench-cargo}} run -q --release --manifest-path bench/host/Cargo.toml -- sweep {{ARGS}}
