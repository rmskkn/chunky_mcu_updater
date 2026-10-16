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

All implementation is under [`poc/`](poc/) (see [`poc/README.md`](poc/README.md)
for the PoC's own map of the demo flow):

- `poc/lib/` — core logic, platform-independent: `flash.c/h` (flash model,
  geometry, meta/journal layout), `manifest.c/h`, `plan.c/h`, `client.c`
  (device-side session), `update_server.c/h` (server-side session),
  `proto.h` (wire messages), `sha256.c/h`, `compress.c`/`lz4.c`, `frame.c/h`,
  `image.c/h`, `util.c/h`.
- `poc/platform/` — the POSIX platform seam (`plat_io_t`, `plat_flash_t`).
- `poc/src/` — entry points: `server.c`, `client.c` (the CLI, distinct from
  `lib/client.c`), `cbflash.c` (the `cb-flash` inspection tool).
- `poc/tests/` — `case_*.sh` integration cases plus `compress_test.c`
  (ASan/UBSan decoder test); `run_all.sh` / `run_matrix.sh` drive them.
- `poc/scripts/` — `lab.sh` (manual two-process demo), `run_demo.sh`
  (scripted walkthrough).

Build/run/test commands: see the `build` skill
(`.claude/skills/build/SKILL.md`).

## Rust workspace

The Rust crates at the top level (`chunky`, `chunky_format`, `chunky_swap`, `chunky_bank`, `chunky_server`,
`chunky_cli`, `chunky_sim`) implement the same protocol side by side with `poc/`; neither links the other. Their
rules (FEATURES.md, CHANGELOG.md, SECURITY.md, SPEC.md, the justfile) are in [`AGENTS_RUST.md`](AGENTS_RUST.md).
