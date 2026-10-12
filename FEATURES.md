# Features

The only status tracker of this repository. Status: ✅ done · 🚧 partial · 📋 planned · 💡 idea · 🐛 bug.
IDs are stable: never renumbered, never reused. New ids come from `tpm work claim AREA`.

## GEO — flash geometry

- ✅ **GEO-1 flash-geometry-asserts** — `FlashGeometry` (chunk, erase, write, STATE/FLOOR/ACTIVE/DFU) checked in a
  `const`: sizes divide each other, a chunk holds the descriptor, the write unit fits the family, partitions are
  aligned and disjoint, DFU and STATE are big enough for the backend, erase size is uniform across each partition
  (`checked_on` with the chip's erase runs). The geometry id travels in manifest, plan and descriptor
  (`GEOMETRY_MISMATCH`). Where the numbers come from is GEO-2; `chip-custom` is the way in for a hand-made one. Bank layouts: ACTIVE and DFU are the same span of two banks, no partition crosses a
  bank, STATE holds the log; written with the swap bit clear, `swapped()` gives the addresses with it set. The
  H743ZI preset is final (BOOT-2); the RP2354 preset gets its final layout with BOOT-3.
- ✅ **GEO-2 flash-series-layout** — `chunky::series`: a `series-*` feature (stm32f1, g0, g4, l4, h7, rp235x)
  picks the series of a firmware build; a build without one (or `chip-custom`) is a compile error. Per line
  (sub-family) the crate knows the write unit and rule, the ECC fault, how a bank is picked, and the erase size
  by density and bank mode (F1 1 KB up to 128 KB and 2 KB above, connectivity line 2 KB; G47x 2 KB dual and
  4 KB single; L4+ 4 KB dual and 8 KB single; H74x/H75x 128 KB, H7A3/H7B3/H7B0 8 KB with 128-bit words; banks
  1 MB apart on H7 and 256 KB apart on G47x). The application gives the `Chip` (line, flash size, bank mode) and
  its `Board` (profile, bootloader and config size, chunk, image size `Max` or `Exact`, factory slot);
  `Layout::checked` computes the partition table in a `const` and every inconsistency is a build error with its
  reason (bank swap on a single-bank part or a line without it, a part that does not exist in that size or
  bank mode, erase size mismatch, uneven sectors, an image or factory slot that does not fit, sizes off the
  erase unit, the RP boot ROM off RP, a swap across two banks that are apart). Profiles: embassy-boot swap, bank
  swap, RP boot ROM, and as layout slots in place with a golden image (BOOT-6) and a factory slot (BOOT-7).
  `embassy_stm32_chip!` reads flash size, erase size, write size and bank layout from embassy-stm32's
  `FLASH_REGIONS`, without a dependency on the HAL. The old presets are worked examples computed this way
  (F103CB, F103CB with golden, G071RB, G0B1RE one bank / bank swap / with factory, H743ZI, H7A3ZI, RP2354).
  Tests: the layout matrix (every line, sizes, bank modes, profiles, factory, config), 40 real parts against
  embassy-stm32's metadata, compile-fail cases with their messages (`chunky/tests/build_errors`), an update and
  a revert of the worked examples on the simulator. Open: `chunky layout` in the CLI for parts that are not a
  preset; the macro is checked by hand against embassy-stm32 0.6.0 (nine chips), a firmware crate in the
  repository will hold it (HW-1); the factory, golden and config regions are not in the geometry id.

## MAN — device manifest

- ✅ **MAN-1 chunk-manifest** — per chunk of ACTIVE and DFU: truncated SHA-256, blank, or unreadable (ECC). Computed
  fresh every session; it is the only progress record, there is no journal. Wire format 1.

## PLAN — plan format

- ✅ **PLAN-1 plan-wire-format** — header with the target's descriptor, then ERASE / COPY / WRITE steps of 24 bytes;
  strict parser, one encoding per plan. Wire format 1.

## APPLY — apply engine

- ✅ **APPLY-1 apply-engine** — runs an untrusted plan against DFU over `embedded_storage`: descriptor first,
  ascending steps, blank-only programming, hash checks on COPY sources and WRITE data, read-back, full image
  verification, then "staged". One chunk of RAM.

## ECC — torn words on ECC flash

- ✅ **ECC-1 try-read-masked** — `EccError` on the flash error, `try_read`, and `Masked` (faulting units read as
  zero) for everything that reads flash a power cut may have torn.
- 🚧 **ECC-2 ecc-fault-handlers** — the real thing on hardware: G0 NMI and H7 BusFault handlers behind
  `EccError`, and a power-cut test on a Nucleo. G0 half written for the power-cut bench (HW-3, B129A):
  `bench_fw_common::ecc` (the NMI checks and clears FLASH_ECCR/ECCR2.ECCD, counts the fault and its address in the
  report; a read that ran into it returns an error with `is_ecc`), not yet run on hardware. Then H7 BusFault, and
  the reader as a library crate a real bootloader can use.

## SEC — security

Design: [SECURITY.md](SECURITY.md).

- ✅ **SEC-1 signed-descriptor** — 128-byte descriptor at the end of ACTIVE (product, version, security counter,
  geometry, SHA-256), Ed25519 with a domain string, strict verification. Format 1.
- ✅ **SEC-2 version-floor** — unary write-once counter in its own partition, raised only by the bootloader when an
  image is confirmed, enforced on every path that picks an image; robust against a single flipped bit.
- 📋 **SEC-3 release-key-handling** — private key on a hardware key, signing apart from building; two key slots
  and a revocation counter; test keys cannot verify on a release build; the signer refuses counters the floor
  cannot hold.
- 📋 **SEC-4 device-manifest-auth** — device identity and session freshness, if the planner needs them at all.
- 📋 **SEC-5 retry-budget-last-result** — per-version retry budget and backoff, last-result reporting, a cap on
  erases per day against a hostile or broken server.
- 📋 **SEC-6 provisioning-protection** — WRP/RDP and friends per family, set and checked; F103 has no RDP2.
- 📋 **SEC-7 security-pass** — the pass on the top model over everything above. The prototype was built on Opus.
- 🚧 **SEC-8 unsigned-build-option** — `ed25519` / `unsigned` features without a default, scheme inside the sealed
  descriptor, signed devices refuse unsigned images, manifest carries the scheme, planner warns and refuses a
  mismatch. Open: flash saved per family (measure), the HELLO field (NET-1), the CLI warning (CLI-1), whether
  CRC32 instead of the image SHA-256 saves anything.

## SIM — simulator

- ✅ **SIM-1 family-flash-sim** — NOR flash with the write rules of F1 (half-word once or zero), G0 (64 bit once,
  ECC), H7 (256 bit once, ECC) and RP2350 (bits to zero); torn writes and erases; ECC faults; a power cut at any
  write or erase; a rule broken by our code panics. Two banks with a SWAP_BANK option bit: addresses are as the
  CPU sees them, the bit takes effect at reset, programming it is an operation a power cut can hit.
- ✅ **SIM-2 power-cut-sweeps** — a cut at every flash operation of a whole update (stage, swap, trial, confirm or
  revert), several ways of tearing each, per family, with chunk equal to and smaller than the erase unit; property
  test with up to four cuts per update. One update loop for every backend (`Device<M, B>`, `tests/common`): the
  swap and the bank backend run through the same checks.
- 📋 **SIM-3 parser-fuzz-proptests** — garbage into every parser and the apply engine at scale; fuzz targets.

## SRV — server

- ✅ **SRV-1 planner** — per erase block: skip, program the blank chunks, or erase and program; COPY from anywhere
  in the running image, else WRITE. Refuses geometry and scheme mismatches. Stats per plan.
- ✅ **SRV-2 blob-store** — content-addressed by chunk hash, in memory and in a directory (checked on read).
- 📋 **SRV-3 rollout-control** — device table, targets per group, known-bad marks, automatic stop of a rollout.
- 💡 **SRV-4 delta-shrinkers** — linker order (stable code first) and compressed WRITE chunks, measured first.

## BOOT — boot backends

- ✅ **BOOT-1 swap-backend** — embassy-boot, unmodified: staged image verified before the swap, ACTIVE verified on
  every boot, one trial boot, revert, floor raised at confirm; ECC faults masked; state erases guarded. Driven end
  to end in the simulator.
- ✅ **BOOT-5 embassy-torn-erase-repro** — stand-alone reproduction of the embassy-boot torn state-erase bug
  (`embassy_torn/`, report in `embassy_torn/REPORT.md`): stock 0.7.0 and git master both lose the images, the
  zero-the-magic-first guard survives every tear. Open: send it upstream.
- ✅ **BOOT-2 bank-swap-backend** — `chunky_bank`: hardware bank swap behind `BankSwitch` (the SWAP_BANK option
  bit). The other bank is verified (image, floor, and that it holds this bootloader) before the bit is programmed,
  ACTIVE on every boot, one trial boot, revert by flipping the bit back after verifying what it goes back to;
  nothing is copied and the old release stays in place. State is a log of write-once flags (one write unit each),
  erased only when used up, header zeroed first. Sweeps green on H7 rules with 2 KB chunks in big sectors, from
  either bank, with a used-up log, on G4-like flash, and on the real STM32H743ZI preset. Simulator only: the
  hardware side is HW-2.
- 📋 **BOOT-3 rp-bootrom-backend** — RP2350/RP2354 boot ROM A/B with try-before-you-buy.
- 💡 **BOOT-6 in-place-golden** — tiny-flash profile (DESIGN §18): one image slot, updated in place by a small
  golden recovery image at its own address that owns the transport, so the bootloader stays transport-free;
  in-slot COPY off in that profile. The layout slot exists (GEO-2, `Profile::InPlaceGolden`, 90 KB for the
  application on an F103CB against 52 KB with A/B); it has no `FlashGeometry` until the backend does.
- 💡 **BOOT-7 factory-restore** — a signed image linked for ACTIVE in a slot of its own, copied in by the
  bootloader as the last resort, and a COPY source for deltas. The layout slot exists (GEO-2, `Board::factory`,
  with every profile; per bank with bank swap) and fails the build where it does not fit; nothing reads or
  protects it yet.
- 💡 **BOOT-4 rot-fallback** — ACTIVE that rots after it was confirmed falls back to the previous image instead of
  recovery. Cheapest on the bank backend, where the previous image is one bit away.

## CLI — command line

- ✅ **CLI-1 chunky-cli** — `chunky` (crate `chunky_cli`): keygen (seed file mode 600, refused when readable by
  others), pubkey, image build/info, manifest, plan (`--from` image or `--manifest`, skip/copy/write stats,
  `-o` plan file), store add/ls, chips, completions (bash, zsh, fish; chip names with help). `--version` has
  the git SHA and build time, colour off without a TTY or with `NO_COLOR`. Key files, `Manifest::of_running_image`
  and `DirStore::list` are in `chunky_server`. Open: the Python wrapper (PY), no progress bar (nothing here takes
  seconds), completion of store ids is not live.

## NET — transport

- 📋 **NET-1 wire-weaver-transport** — the update API over wire_weaver (RTT first), no COM ports.

## HW — hardware

- 📋 **HW-1 g0-nucleo-bringup** — bootloader binary and example app on a Nucleo G0B1RE; sizes and timings.
- 📋 **HW-2 h7-nucleo-bank-bringup** — `BankSwitch` over the H7 option bytes, the bootloader in both banks, an
  example app on a Nucleo H743ZI. To confirm there: option byte programming cut by power loss leaves the old or
  the new SWAP_BANK value and nothing else; zeros over a used flash word; timings. Then the parts that pick the
  bank another way (G4 and L4: BFB2 and the system bootloader).
- 🚧 **HW-3 power-cut-bench-fw** — B129A (STM32G0B1CE) test firmware in `bench/fw`: bootloader `stock`
  (embassy-boot 0.7.0 as it comes) and `guarded` (`chunky_swap::Guarded`, G0 ECC NMI reader), application images
  A and B (filler so every page differs), PA6 high right before each STATE erase after `pre_trigger_us`, modes run
  / measure / provision from a config record in flash, a report record in RAM. Layout and records:
  `bench/bench_layout`; wiring, run order and the measurement procedure: `bench/README.md`. P2625#11oct-1213.
  Built (`just bench-fw`: four images and ELFs in `bench/fw/out`, sizes in `bench/README.md`); bring-up on the
  bench is next (M0-M3 of the procedure).
- 🚧 **HW-4 power-cut-bench-harness** — `chunky-bench` in `bench/host` (own workspace; library `chunky_bench`,
  thin CLI): the glitcher (`glitch` git dep, push-pull power cut on Donguru Q101) and probe-rs (Donguru CMSIS-DAP
  v2, no hidapi), restores S0 page-wise, cuts after a swept delay from the PA6 trigger, reads back
  ACTIVE/DFU/STATE, report, IPSR, ECCR and SR, classifies old/new/mixed/lost/brick with `embassy_torn::classify`;
  `snapshot`, `measure`, `run`, resumable `sweep` (coarse, then fine around outcome changes) into a CSV with raw
  dumps. Tested end to end on a simulated bench (real embassy-boot 0.7.0 on a G0 flash model, timed ECC tears).
  Left: bring-up on the bench (with HW-3), decoding ECCR's address field.

## PY — Python

- 📋 **PY-1 python-wrapper** — `chunky_py` over the server and the simulator.

## DEV — repository

- ✅ **DEV-1 repo-dev-rules** — AGENTS.md, CLAUDE.md, FEATURES.md, CHANGELOG.md, SECURITY.md, justfile with
  default, test, lint, install, deploy.

## Known gaps and findings

- embassy-boot's state-page erase can leave "swap" standing after a power cut (found by the G0 sweep, guarded in
  `chunky_swap::Guarded`); reproduced on stock 0.7.0 and master with a minimal model, report ready (BOOT-5).
  Both `prepare_boot`'s revert and `set_magic` (mark_booted) erase that way. See SECURITY.md "Findings so far".
- After a confirmed update DFU holds the old image shifted by one page, so the next update erases and rewrites
  every DFU block even when little changed: no airtime (COPY), but one erase cycle per block per update, on top of
  the swap's two. Inherent to the swap backend; the bank and boot ROM backends do not have it.
- The bank backend rests on one thing the simulator assumes and does not prove: a power cut during option byte
  programming leaves the old or the new SWAP_BANK value (HW-2). Every other cut is swept.
- SWAP_BANK moves the whole bank, the bootloader's sector too: the bootloader must be in both banks, the same
  bytes, both write-protected (SEC-6). `chunky_bank` refuses to switch when the copies differ.
- Bank backend: a power cut during the trial boot spends the trial (as on the swap backend); the server offers
  the release again, and it is still in the other bank, so that costs no transfer. When the log is started over
  right after a revert (once in 511 updates on H743ZI), that revert reads as "confirmed".
- The series rules (GEO-2) come from the reference manuals and from embassy-stm32's chip metadata, not from
  hardware. Not in that metadata, so from the manuals alone: the 128-bit flash word of H7A3/H7B3/H7B0 (the HAL
  writes 32 bytes there, and so does the layout by default), the ECC fault per series (NMI on G0/G4/L4, bus fault
  on H7), BFB2 as the way G4 and L4 pick a bank, and dual-bank mode on a 512 KB G0B1 being a choice (embassy
  lists it as dual only). HW-1 and HW-2 confirm them.
- A planner that trusts blank chunks (the default where a chunk is smaller than an erase unit) programs into
  flash whose erase may have been cut short. The image hash catches wrong data at once, not marginal cells later.
