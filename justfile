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

# The no_std crates build for Cortex-M0+ and M4F, signed and unsigned; and fail without a choice
check-fw:
    cargo check -q -p chunky_swap --target thumbv6m-none-eabi --features ed25519,chunky/chip-custom
    cargo check -q -p chunky_swap --target thumbv6m-none-eabi --features unsigned,chunky/chip-custom
    cargo check -q -p chunky_swap --target thumbv7em-none-eabihf --features ed25519,chunky/chip-custom
    cargo check -q -p chunky_swap --target thumbv7em-none-eabihf --features unsigned,chunky/chip-custom
    cargo check -q -p chunky_bank --target thumbv6m-none-eabi --features ed25519,chunky/chip-custom
    cargo check -q -p chunky_bank --target thumbv7em-none-eabihf --features ed25519,chunky/chip-stm32h743zi
    cargo check -q -p chunky_bank --target thumbv7em-none-eabihf --features unsigned,chunky/chip-stm32h743zi

# Build everything in release mode and install the `chunky` binary (cargo bin dir)
install:
    cargo build --release --workspace --locked
    cargo install --path chunky_cli --locked

# Bring this PC up to main: pull (refuses on local changes), then install
deploy:
    git pull --ff-only
    just install
