# Agent entry point

Read these two files in full before touching anything else in this repo:

1. [`README.md`](README.md) — what this project is, the idea, the status.
2. [`DESIGN.md`](DESIGN.md) — the specification: wire protocol, flash layout,
   security model, failure matrix, decisions and tradeoffs. Section numbers
   referenced elsewhere in the repo (code comments, commit messages) point
   into this file.

Do not start implementation, review, or design work from assumptions about
sector hashing, the manifest format, flash geometry, or the protocol state
machine without checking the relevant DESIGN.md section first — those
invariants are specified there, not rederivable from skimming the code.

## Where the code lives

The implementation is at the repository root (see [`docs/poc.md`](docs/poc.md)
for the PoC's own map of the demo flow):

- `lib/` — the core, free of platform, host and app code. It is built
  seeing only itself and the generated geometry, and links nothing but
  `memcpy`/`memset`/`vsnprintf` plus the three `plat_flash_*` functions of
  its own port contract. `flash.c/h` (flash model, meta/journal layout),
  `flash_port.h` (the opaque flash port lib/ needs; a port implements it),
  `flash_view.c` (everything that reads the image or manifest out of flash),
  `manifest.c/h` (body, digest, MAC, checks, `sector_hash`), `plan.c/h`,
  `device.c/h` (the update session, fed one message at a time, plus the
  application's confirm; no threads, transport or clock), `bootloader.c/h`
  (the bootloader's apply/verify/trial boot/unroll/revert; includes no
  session or protocol code), `stats.h` (`device_stats_t`, shared by both;
  `clock_ms` is the caller's), `protocol.h` (the pure wire protocol),
  `image.c/h` (descriptor, hashes, per-sector table and build over RAM),
  `compress.c/h` (registry only, no compressor built in), `sha256.c/h`,
  `util.c/h` (CRC-32, `cb_err_t`, log sink).
- `middleware/` — everything between the core and a host program:
  `frame.c/h` and `platform_io.h` (framing over the transport seam),
  `client_session.c/h` and `server_session.c/h` (each side's session over a
  framed link), `flash_file.c/h` (the mmap'd-file flash port: create,
  provision, install the factory image, open by path), `host.c/h` (files,
  randomness, clock, environment), `lz4.c/h` (liblz4 binding;
  `lz4_register()`).
- `platform/` — the POSIX transport: TCP behind `plat_io_t`.
- `device/` — device catalog, one dir per part: `<part>/device.toml`
  (flash size, erase unit, map of bootloader/identity/meta/slot A/slot B,
  program rules, timing). `tools/gen_device.py` turns it into
  `build/device/geometry.h` (included by `lib/flash.h`, which re-checks it)
  and `build/device/memory.ld`. Picked with `make DEVICE=...`; edit the
  TOML, never the generated files.
- `src/` — the CLIs: `server.c`, `client.c`, `cbflash.c` (the
  `cb-flash` inspection tool), `cli.h` (`cli_fail()`, how a CLI command
  reports an error), `keys.h` (PoC key material).

Errors: nothing exits on an error. Fallible calls return `-1` (or a
negative/`DEV_*` code) with the reason in a `cb_err_t` (`lib/util.h`),
`flash_t.err`, or a `why` string; only a CLI's `main()` turns a failure into
an exit status. There is no `die()`; don't add one.
- `tests/` — `case_*.sh` integration cases plus `compress_test.c`
  (ASan/UBSan decoder test) and `auth_test.c` (drives the session and the
  bootloader with forged messages; run by `case_auth.sh`); `run_all.sh` /
  `run_matrix.sh` drive them.
- `SPEC.md`, `rust/` — format 2 in octets, and its Rust types: `rust/chunky_format` (the types, the golden
  `vectors/`, the generated C reader `c/chunky_format.h` with its stamp, `c/test.c` and the example
  `c/read_plan.c`) and `rust/chunky_format_gen` (writes and checks the header). Never edit
  `chunky_format.h` by hand: change the types, `make regen`, commit header and stamp; `make check-regen` and
  `make rust-test` say when it is stale.
- `scripts/` — `lab.sh` (manual two-process demo), `run_demo.sh`
  (scripted walkthrough).

Build/run/test commands: see the `build` skill
(`.claude/skills/build/SKILL.md`).
