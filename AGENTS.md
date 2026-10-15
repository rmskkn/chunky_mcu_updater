# Working on chunky_mcu_updater

Guidance for AI agents and contributors. Read this before changing code.

chunky is a delta firmware updater for small microcontrollers: the device reports a hash per flash chunk, the
server sends only what changed (or tells the device to copy it from its running image), and a boot backend
switches to the new image, gives it one trial boot and reverts when it does not confirm.

This is our fork (`origin` = romixlab) of Roman Skakun's design (`upstream` = rmskkn, design only). **Never push to
`upstream`.** [DESIGN.md](DESIGN.md) is the original design and stays as he wrote it; where the code differs
(embassy-boot instead of a custom bootloader, no journal, chunks instead of sectors, truncated SHA-256, Ed25519),
[README.md](README.md) and [SECURITY.md](SECURITY.md) say how. The review that led there is in
`~/git/tpm/prj/P2625-chunky-ota/notes.md`. Project: tpm P2625.

## FEATURES.md is the source of truth

[FEATURES.md](FEATURES.md) lists every feature with its status, every known bug and what is planned, with stable
IDs per area (`GEO`, `MAN`, `PLAN`, `APPLY`, `ECC`, `SEC`, `SIM`, `SRV`, `BOOT`, `CLI`, `NET`, `HW`, `PY`, `DEV`).

- Read the relevant area before starting. A "new" bug or idea is often already recorded.
- Name IDs with a short slug when talking to the user: `SEC-2 version-floor`, never a bare `SEC-2`. Commit
  messages, CHANGELOG and code `TODO`s keep the bare ID.
- Update it in the same commit as the code. New ids: `tpm work claim AREA` prints the next free one. Never
  renumber or reuse IDs.
- Don't track status anywhere else. Code `TODO`s that matter reference an ID: `// TODO(BOOT-2): ...`.

## CHANGELOG.md records every change

- Every change a user would notice gets an entry under `## [Unreleased]` in the same commit: `### Added`,
  `### Changed`, `### Fixed`, `### Removed`, with the feature ID in parentheses.
- Wire format changes (manifest, plan, descriptor) are always recorded, with the format number.
- Session commits never bump the version and are never pushed; `tpm land` turns `[Unreleased]` into a version
  section, bumps `[workspace.package] version` and tags it. Every version gets its own section.

## SECURITY.md is the security design

Firmware signing, anti-rollback and the plan parser make this repository security-sensitive. [SECURITY.md](SECURITY.md)
says what is trusted, who may write which flash region and what each attacker can and cannot do. A change that
moves one of those lines changes SECURITY.md in the same commit and is planned on the top model first.

- A vulnerability found on the way becomes a tpm task on P2625 tagged `+security` with a severity word.
- No private key is ever committed, logged or printed. Tests derive throwaway keys from fixed seeds.
- Code that reads bytes from outside (plan, manifest, descriptor) never panics on them, never indexes without a
  length check, and has a test that feeds it garbage.

## Layout

Cargo workspace, edition 2024. Pure-Rust crates only (no `-sys`, no `cc`/`bindgen`): check `cargo tree` when
adding a dependency.

- `chunky/` — the core, `no_std`, no `unsafe`. `geometry.rs` (chunk, erase and write sizes and the partition
  table, const-checked), `series.rs` (the flash rules per MCU series behind the `series-*` features, and the
  const fn that lays out a part for a profile; a firmware build without a series feature fails), `chips.rs`
  (worked examples of it, the presets of the host tools), `descriptor.rs`
  (the sealed 128-byte image descriptor, `Trust` as the seal check, verify before decode), `floor.rs` (the version floor counter), `manifest.rs`,
  `plan.rs` (wire formats), `apply.rs` (the engine that runs an untrusted plan), `ecc.rs` (`EccError`,
  `try_read`, `Masked`), `part.rs` (`Part`, `ReadOnly`), `backend.rs` (`BootBackend`).
- `chunky_swap/` — the embassy-boot backend, `no_std`: `boot()` is the bootloader's whole decision,
  `SwapUpdater` the application's handle, `Guarded` the state partition as embassy-boot gets it.
- `chunky_bank/` — the hardware bank swap backend, `no_std`: `boot()` again the whole decision, `BankUpdater`
  the application's handle, `BankSwitch` the SWAP_BANK option bit, `log.rs` the state log of write-once flags.
- `chunky_format/` — the wire format as fixed-layout and head+tail shrink_wrap types (format 2, PROTO-3:
  descriptor, plan header, steps, manifest), what `chunky` reads and `chunky_server` writes: `signed!`, golden
  `vectors/` (made by `chunky_server::vectors`, `just vectors`), the generated C reader in `c/`, `size_bench/`
  (own nightly workspace). Needs `../wire_weaver` (SW-44), and so does every crate above it.
- `chunky_server/` — host side: `image.rs` (build and seal), `store.rs` (blob store), `manifest.rs` (parse),
  `planner.rs`, `vectors.rs` (the one generator of the golden vectors and `c/golden.h`).
- `chunky_sim/` — `flash.rs` (NOR flash with per-family write rules, torn writes and erases, ECC faults),
  `device.rs` (a whole device on either backend, `Device<M, B>`), `tests/` (power-cut sweeps, security cases,
  planner properties; `tests/common` is the update loop they share).
- `bench/` — the hardware power-cut bench (HW-3, HW-4, `bench/README.md`): `bench_layout` (the contract) and
  `fw_image` are workspace members; `bench/fw` is the B129A firmware in its own thumbv6m workspace
  (`just bench-fw`, linted by `just lint`).

A tool that drives a device is a library first: nothing the CLI can do is missing from `chunky_server`.

## How images are verified is a build option

`chunky`, `chunky_swap` and `chunky_bank` have no default features. A build picks `ed25519` (signed releases) or
`unsigned` (the explicit opt-out for small flash: SHA-256 integrity and the version floor stay, authenticity
goes). Neither is a compile error; so are both on a firmware target. Host crates enable both so they can build,
plan and test either kind. Never add a default, and never make a code path that accepts an unsigned image where
a key is trusted.

## Flash rules the code must keep

They hold on every family, and the simulator panics when one is broken:

- A write unit is programmed once after an erase. The only second write that is ever allowed is all zeros.
- Every flag is one write unit, written once (embassy-boot's progress words, the floor counter).
- On ECC flash a write or erase cut by power loss leaves words that fault when read. Anything that reads flash
  that may be torn goes through `try_read` or `Masked`.
- An erase is never assumed to finish. If what it erases means something while half gone, neutralise it first
  (see `Guarded`).

## Commands

```sh
just            # list recipes
just test       # everything; CHUNKY_SWEEP_SEEDS=500 just test digs deeper
just lint       # clippy -D warnings, fmt --check, firmware-target builds
cargo test -p chunky_sim --test sweep
```

Before declaring a change done: `just lint` and `just test`. A change to the apply engine, the swap backend, the
floor or the simulator's flash model also runs the sweeps with `CHUNKY_SWEEP_SEEDS=500` once.

## Tests

Automated only. The sweeps cut power at every flash operation of a whole update, for every family model, and
check after each boot that the device runs a verified release. A new backend plugs into the same loop. A new
behaviour gets its test in the same commit; a finding the tests make in embassy-boot or in our code is written
down in FEATURES.md.

## Hardware and transport (later)

Firmware talks to the PC over wire_weaver (RTT through the debug probe first), never a text protocol on a COM
port. The first board is a Nucleo G0B1RE (`wire_weaver/examples/nucleo_g0b1re`).

## Commits

Small commits on the session branch, imperative subject, the feature ID where one applies. Never push to
`upstream`; our branches land through `tpm land`.
