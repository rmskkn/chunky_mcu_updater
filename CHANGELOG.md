# Changelog

All notable changes to this project are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/),
and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

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
