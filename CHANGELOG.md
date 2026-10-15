# Changelog

All notable changes to this project are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/),
and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed

- Wire format 2 everywhere: `chunky` reads and `chunky_server` writes the descriptor, plan and manifest of
  `chunky_format` (descriptor `CKY2`, plan `CKP2`, manifest `CKM2`); format 1 is gone, and images built before
  are not read any more. `chunky` now depends on wire_weaver checked out next to this repository. (PROTO-3)
- Descriptor format 2: a 64-byte body and a 64-byte seal over `chunky image descriptor v2` and exactly the body's
  bytes. `Trust` implements `chunky_format::SealCheck`; `open_descriptor` checks the seal before it decodes a field,
  then scheme, product, geometry, length, floor. An unsigned image on a signed device is now refused as
  `BadSignature` (the seal fails first), `WrongScheme` means a sealed body that names another scheme.
  `ImageDescriptor` is `chunky_format::DescriptorBody`; `ImageDescriptor::parse` is gone (hosts:
  `chunky_server::image::read_descriptor_unverified`). (SEC-1, PROTO-3)
- Chunk hashes are SHA-256 truncated to 8 bytes (`ChunkHash`, the manifest, COPY and WRITE, the blob store's
  names). (MAN-2)
- Manifest format 2: a 13-byte header and one entry per chunk, `0` + hash, `1` blank, `2` unreadable.
  `chunky_server::Manifest::encode` writes it. (MAN-2)
- The apply engine reads format 2 steps with `Step::view`: `Applier::step` takes exactly one encoded step (a
  WRITE with its chunk, no separate data argument), `Applier::step_len` frames a step from its head and refuses a
  WRITE that is not one chunk before its data. New `ApplyError::BodyLength`: a header body length beyond the
  geometry, or steps that do not add up to it. (APPLY-1)
- The planner's steps are `chunky_server::planner::PlanStep` (a WRITE owns its chunk); the header carries the body
  length. (SRV-1)
- `plan_size` is an example of `chunky_server` and prints format 2 only; `just size-bench` drops format 1.
  (PROTO-3)

### Added

- `chunky_format`: `ManifestHeader`, `ChunkEntry`, `step_len_from_head`, `WRITE_COUNT_OFFSET`; the C header has
  the manifest readers. (MAN-2, PROTO-3)
- `chunky_sim`: `Device::apply_steps` (one message per step); security cases for a lying body length, a step that
  is not exactly one step, a WRITE count beyond a chunk, a key-signed body naming another scheme, a signed image
  on an unsigned device. (SIM-2)

## [0.9.0] - 2026-10-15

### Changed

- `chunky_format`: `Step` is a `head_tail` enum with a typed `List<u8>` for a WRITE's data, read with `Step::view`
  (`read_step`, `step_len`) instead of `StepRead` and the `read_step_fixed` / `read_step_sw` glue. Wire bytes of
  format 2 are identical (`vectors/` and `c/golden.h` unchanged); the C reader is regenerated (step accessors take
  the variant head, `chunky_step_tail` walks the data). Plan reader on Cortex-M0 / RV32IMC: 734 / 912 bytes, was
  892 / 1086 (format 1 hand-written: 840 / 1252). (PROTO-3)

## [0.8.0] - 2026-10-14

### Added

- `chunky_format`: the plan format as shrink_wrap types with a fixed little-endian layout, format 2 (descriptor
  `CKY2`, plan `CKP2`, steps sized per op, 8-byte chunk hashes, an explicit WRITE data length), the `signed!`
  envelope (verify, then decode), golden vectors in `chunky_format/vectors`, and the C reader generated from the
  types (`chunky_format/c`, checked by `just test-c`, part of `just test`). Not yet read by `chunky`, which stays on
  format 1. Needs wire_weaver checked out next to this repository. (PROTO-3)
- `just size-bench`: code size of the plan readers on Cortex-M0 and RV32IMC; `chunky_format`'s `plan_size` example:
  a plan's size in format 1 and 2 for every preset geometry. (PROTO-3)

## [0.7.0] - 2026-10-13

### Added

- Bench results on a B129A (`bench/results`): a reset 62 us into the STATE erase bricks stock embassy-boot
  0.7.0 (ECC double error, 4 of 8 runs); `chunky_swap::Guarded` with the G0 ECC NMI reader survives 272 of 272
  runs across that window. `chunky-bench --raw-state`; outcome `brick:stuck`. (HW-4, ECC-2, BOOT-5)
- `chunky-bench --cut reset --width W`: the cut as an NRST pulse (glitcher output open-drain on PF2-NRST)
  instead of a power cut; every restart of a run is then a reset pulse. CSV columns `cut` and `width_us`. A
  delay below the glitcher's minimum (3 clock cycles) is raised to it instead of failing. (HW-4)
- Bench firmware: status LED on PB14, solid on whenever the board has power; `bench/README.md` wiring for the glitcher pins H6/H8/G8 and the M2K channels. (HW-3)
- `bench/`: the hardware power-cut bench for embassy-boot's STATE erase on a B129A (STM32G0B1CE): wiring, run
  order and measurement procedure in `bench/README.md`; `bench_layout`, the flash and RAM layout and the config
  and report records the test firmware and the harness share. (HW-3, HW-4)
- `bench/fw`: the B129A bench firmware, its own thumbv6m workspace: bootloaders `stock` and `guarded` (embassy-boot
  0.7.0, STATE plain or behind `chunky_swap::Guarded`), application images A and B (run, measure, provision), the
  PA6 trigger on the raw erase. `just bench-fw` writes `boot-stock`, `boot-guarded`, `app-a`, `app-b` (`.bin`,
  apps padded to 32 KB, and `.elf`) to `bench/fw/out`; `just lint` builds and lints it. (HW-3)
- The G0 ECC NMI reader for the bench: the NMI handler clears FLASH_ECCR.ECCD and counts the fault, the flash read
  that hit it returns an `EccError`. (ECC-2)
- `chunky-bench` (`bench/host`, own workspace): the power-cut bench harness. Library `chunky_bench` with the
  glitcher as power switch and probe-rs as SWD access behind traits, page-wise restore of snapshot S0, read-back
  of report, IPSR, FLASH_ECCR/SR and STATE/ACTIVE/DFU, classification, resumable sweeps (coarse grid, then a fine
  one around outcome changes) into a CSV with raw dumps, and a simulated bench for tests and `--dry-run`.
  Commands `status`, `cycle`, `snapshot`, `measure`, `run`, `sweep`; recipes `bench-test`, `bench-lint` (part of
  `lint`), `bench-measure`, `bench-sweep`. (HW-4)
- `embassy_torn::classify`: the old/new/mixed/lost classification as pure functions on pages, shared by the
  model's tests and the bench. (HW-4)

## [0.6.0] - 2026-10-11

### Added

- `chunky::series`: flash layouts per MCU series. A `series-*` feature (`series-stm32f1`, `-stm32g0`,
  `-stm32g4`, `-stm32l4`, `-stm32h7`, `series-rp235x`) carries the rules of the series; the application gives
  its `Chip` (line, flash size, bank mode) and `Board` (profile, bootloader, config, chunk, image size, factory
  slot), and `Layout::checked` computes the partition table in a `const`. Every inconsistency is a build error
  with its reason: bank swap on a single-bank part, a part that does not exist in that size or bank mode, erase
  size mismatch, uneven sectors, an image or factory slot that does not fit. (GEO-2)
- Layout slots for the profiles to come: in place with a golden image (`Profile::InPlaceGolden`, BOOT-6) and a
  factory restore slot (`Board::factory`, BOOT-7). Their backends do not exist yet. (GEO-2)
- `embassy_stm32_chip!`: the part from embassy-stm32's generated flash metadata (flash size, erase size, write
  size, bank layout), without `chunky` depending on the HAL. (GEO-2)
- Worked examples in `chunky::chips`: STM32G071RB, STM32G0B1RE with bank swap and with a factory slot,
  STM32F103CB with a golden image, STM32H7A3ZI; `chips::LAYOUTS` lists them with every region. `chunky chips`
  and `--chip` know `stm32g071rb`, `stm32g0b1re-bank` and `stm32h7a3zi`. (GEO-2)

### Changed

- The `chip-stm32f103cb`, `chip-stm32g0b1re`, `chip-stm32h743zi` and `chip-rp2350` features are replaced by the
  `series-*` features, and `chips::SELECTED` by the application's own `Layout`. `chip-custom` stays. (GEO-2)
- The presets are computed from the series rules. STM32F103CB and STM32H743ZI keep their numbers and geometry
  ids. STM32G0B1RE (ACTIVE 240 KB at 30 KB instead of 238 KB at 32 KB) and the RP preset (two slots of 988 KB,
  now named `rp2354`, before `rp2350-2mb`) get new numbers and with them new geometry ids. No wire format
  changed. (GEO-2)
- `Family::Stm32H7` accepts a write unit of 16 bytes (the 128-bit flash word of H7A3/H7B3/H7B0) next to 32.
  (GEO-1)

### Removed

- `chips::*_FLASH` erase runs: `Layout::runs` gives them. (GEO-2)

## [0.5.0] - 2026-10-11

### Added

- `chunky` command line (crate `chunky_cli`): `keygen`, `pubkey`, `image build|info`, `manifest`, `plan`,
  `store add|ls`, `chips`, `completions`; `--version` shows git SHA and build time. (CLI-1)
- `chunky_server::keys` (release key file, mode 600, refused when others can read it),
  `Manifest::of_running_image`, `DirStore::list`. No wire format changed. (CLI-1)

## [0.4.0] - 2026-10-11

### Added

- `chunky_bank`: boot backend on hardware bank swap (dual-bank STM32 with a SWAP_BANK option bit). The other
  bank's image, its version floor and its bootloader copy are verified before the bit is programmed, ACTIVE on
  every boot; one trial boot; revert by flipping the bit back. Its state is a log of write-once flags that is
  erased only when used up. `BankSwitch` is the option bit, `BankMap` the addresses for the current mapping.
  (BOOT-2)
- Simulator: two banks with a SWAP_BANK option bit (`SimFlash::with_banks`, `program_swap`, `reset`), and the
  simulated device on either backend (`Device<M, Bank>`, `bank_layout`). (SIM-1)
- Power-cut sweeps for the bank backend: H7 rules with 2 KB chunks in big sectors, from either bank, with a
  used-up state log, G4-like flash, and the real STM32H743ZI layout; security cases for forged state, rollback
  through the other bank and a missing bootloader copy. (SIM-2, BOOT-2)

### Changed

- Bank geometries are checked for what bank swap needs: ACTIVE and DFU the same span of two banks, no partition
  across both, STATE big enough for the log (`GeometryError::BankLayout`). The STM32H743ZI preset keeps its
  numbers and its geometry id; they are now final. No wire format changed. (GEO-1)
- `chunky_sim::Device` is generic over the boot backend (`Device<M, B = Swap>`), and the update loop of the
  tests runs any backend. `Device<M>` is the swap device as before. (SIM-2)

## [0.3.0] - 2026-10-11

### Added

- `embassy_torn`: minimal NOR flash model with torn erases (word, bit and ECC tears) and a test suite that runs
  stock embassy-boot 0.7.0 and git master through update, swap, trial boot, revert and confirm with power cut
  during every state erase; shows the lost-images bug and that zeroing the magic first fixes it. (BOOT-5)

## [0.2.0] - 2026-10-11

### Added

- Cargo workspace with the first prototype, host-first: `chunky` (no_std core), `chunky_swap` (embassy-boot
  backend), `chunky_server` (image builder, blob store, planner), `chunky_sim` (flash simulator and a simulated
  device). (DEV-1)
- `FlashGeometry` with separate chunk, erase and write sizes, checked in a `const`: a layout that cannot work
  stops the build with the reason. Presets for STM32F103CB, STM32G0B1RE, STM32H743ZI and RP2350; a firmware
  build without a chip feature is a compile error. (GEO-1)
- Device manifest of per-chunk truncated SHA-256 with blank and unreadable chunks, wire format 1. (MAN-1)
- Plan wire format 1 (ERASE, COPY from the running image, WRITE) with a strict parser. (PLAN-1)
- Apply engine over `embedded_storage`: ordered steps, programs only blank chunks, checks every COPY source and
  WRITE against its hash, reads back, verifies the whole image before switching. (APPLY-1)
- `EccError`, `try_read` and `Masked`: reads that survive ECC faults from torn writes. (ECC-1)
- Signed image descriptor (Ed25519, product, version, security counter, geometry, SHA-256), format 1. (SEC-1)
- Version floor: a unary write-once counter raised by the bootloader when an image is confirmed. (SEC-2)
- `ed25519` / `unsigned` build option: signing can be left out on small flash by name, never by default; a
  device that trusts a key refuses unsigned images; the planner warns about unsigned devices. (SEC-8)
- embassy-boot swap backend: staged image verified before the swap, ACTIVE verified on every boot, one trial
  boot, revert; state-page erases guarded against power cuts. (BOOT-1)
- Flash simulator with F1, G0, H7 and RP2350 write rules, torn writes and erases, ECC faults, and a power cut
  at any operation. (SIM-1)
- Power-cut sweeps: a cut at every flash operation of a whole update (stage, swap, trial, confirm or revert) per
  family, plus property tests with several cuts. (SIM-2)
- Planner with skip / COPY / WRITE rules per erase block, memory and directory blob stores. (SRV-1, SRV-2)
- Repo dev rules: AGENTS.md, CLAUDE.md, FEATURES.md, SECURITY.md, justfile. (DEV-1)
