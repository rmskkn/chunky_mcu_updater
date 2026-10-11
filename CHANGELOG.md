# Changelog

All notable changes to this project are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/),
and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

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
