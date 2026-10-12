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
    just lint-bench-fw
    just bench-fw

# The no_std crates build for Cortex-M0+ and M4F, signed and unsigned, with a series-* feature or chip-custom
check-fw:
    cargo check -q -p chunky_swap --target thumbv6m-none-eabi --features ed25519,chunky/series-stm32g0
    cargo check -q -p chunky_swap --target thumbv6m-none-eabi --features unsigned,chunky/chip-custom
    cargo check -q -p chunky_swap --target thumbv7em-none-eabihf --features ed25519,chunky/series-stm32g4
    cargo check -q -p chunky_swap --target thumbv7em-none-eabihf --features unsigned,chunky/chip-custom
    cargo check -q -p chunky_bank --target thumbv6m-none-eabi --features ed25519,chunky/chip-custom
    cargo check -q -p chunky_bank --target thumbv7em-none-eabihf --features ed25519,chunky/series-stm32h7
    cargo check -q -p chunky_bank --target thumbv7em-none-eabihf --features unsigned,chunky/series-stm32h7

# Power-cut bench firmware (HW-3): both bootloaders and apps A and B, raw images and ELFs in bench/fw/out
bench-fw:
    just _bench-fw bench_bootloader stock boot-stock boot
    just _bench-fw bench_bootloader guarded boot-guarded boot
    just _bench-fw bench_app image-a app-a app-a
    just _bench-fw bench_app image-b app-b app-b

_bench-fw pkg feature name kind:
    mkdir -p bench/fw/out
    cd bench/fw && cargo build -q --release -p {{pkg}} --features {{feature}}
    cp bench/fw/target/thumbv6m-none-eabi/release/{{pkg}} bench/fw/out/{{name}}.elf
    cargo run -q -p bench_fw_image -- bench/fw/out/{{name}}.elf bench/fw/out/{{name}}.bin {{kind}}

# clippy and formatting of the bench firmware workspace, every build of it
lint-bench-fw:
    cd bench/fw && cargo fmt --check
    cd bench/fw && cargo clippy -q --release -p bench_bootloader --features stock -- -D warnings
    cd bench/fw && cargo clippy -q --release -p bench_bootloader --features guarded -- -D warnings
    cd bench/fw && cargo clippy -q --release -p bench_app --features image-a -- -D warnings
    cd bench/fw && cargo clippy -q --release -p bench_app --features image-b -- -D warnings

# Build everything in release mode and install the `chunky` binary (cargo bin dir)
install:
    cargo build --release --workspace --locked
    cargo install --path chunky_cli --locked

# Bring this PC up to main: pull (refuses on local changes), then install
deploy:
    git pull --ff-only
    just install
