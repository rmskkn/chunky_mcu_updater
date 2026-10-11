# Features

The only status tracker of this repository. Status: ✅ done · 🚧 partial · 📋 planned · 💡 idea · 🐛 bug.
IDs are stable: never renumbered, never reused. New ids come from `tpm work claim AREA`.

## GEO — flash geometry

- ✅ **GEO-1 flash-geometry-asserts** — `FlashGeometry` (chunk, erase, write, STATE/FLOOR/ACTIVE/DFU) checked in a
  `const`: sizes divide each other, a chunk holds the descriptor, the write unit fits the family, partitions are
  aligned and disjoint, DFU and STATE are big enough for the backend, erase size is uniform across each partition
  (`checked_on` with the chip's erase runs). Presets for STM32F103CB, G0B1RE, H743ZI, RP2350; a firmware build
  without a `chip-*` feature is a compile error; the geometry id travels in manifest, plan and descriptor
  (`GEOMETRY_MISMATCH`). Bank layouts: ACTIVE and DFU are the same span of two banks, no partition crosses a
  bank, STATE holds the log; written with the swap bit clear, `swapped()` gives the addresses with it set. The
  H743ZI preset is final (BOOT-2); the RP2350 preset gets its final layout with BOOT-3.

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
- 📋 **ECC-2 ecc-fault-handlers** — the real thing on hardware: G0 NMI and H7 BusFault handlers behind
  `EccError`, and a power-cut test on a Nucleo.

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
- A planner that trusts blank chunks (the default where a chunk is smaller than an erase unit) programs into
  flash whose erase may have been cut short. The image hash catches wrong data at once, not marginal cells later.
