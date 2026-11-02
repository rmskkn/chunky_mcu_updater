# format 2 (SPEC.md): reading it from this tree, read-only

This is a plain record of what landed as new files, for anyone orienting themselves later: every file below
is new; nothing in `lib/`, `middleware/`, `src/`, the `Makefile` or `tests/run_all.sh` was changed to add it.

## What this is, and what it is not

[`SPEC.md`](../SPEC.md) and [`rust/chunky_format/c/chunky_format.h`](../rust/chunky_format/c/chunky_format.h)
are vendored, byte-for-byte, from
[romixlab/chunky_mcu_updater PR #5](https://github.com/rmskkn/chunky_mcu_updater/pull/5) (commit
`b2c3f4b0e96090f183095b61cb6680cda66dba10`): the "format 2" wire contract -- a different, newer protocol
than the one this repo implements today (`lib/manifest.h`'s `manifest_t`, `lib/plan.h`'s `plan_t`,
`lib/protocol.h` -- DESIGN.md sections 4-6). The two are not byte-compatible and this integration does not
try to make them so: `manifest_build()`, `plan_build()`, `bootloader_boot()` and everything that calls them
are untouched and unaware format 2 exists.

[`lib/format2.h`](../lib/format2.h) / [`lib/format2.c`](../lib/format2.c) is new, additive code: a reader and
verifier for format-2 buffers, built only on `chunky_format.h`'s offset accessors and the SHA-256 already in
`lib/sha256.h`. It exists so a format-2 buffer -- a plan or manifest from a format-2-capable peer -- can be
decoded and checked in this tree; it is not wired into `device.c`, `bootloader.c` or either session layer,
and does not need to be to be useful (it also works as a one-off inspection/validation tool).

## What `lib/format2.c` actually checks

Follows SPEC.md section 6's order deliberately, not just its fields:

1. `format2_plan_begin()`: every refusal that needs no key first (magic, format, geometry id, body length vs.
   the stream, the raw descriptor body's stage-1 fields -- SPEC.md 4.3 stage 1), **then** the seal over the
   exact 64 sealed body bytes (stage 2), **then** product id / geometry id / rollback floor, read only from
   the now-sealed bytes (stage 3). A caller never gets a product id or security counter it hasn't paid a seal
   check for.
2. `format2_plan_walk()`, per step (SPEC.md 6.2): a WRITE's byte count must equal the geometry's `chunk_len`
   (`FORMAT2_EDATALEN`); every index must be in range, `dst`/`src` < `chunks` and an ERASE's block < `blocks`
   (`FORMAT2_ERANGE`); a step's position must strictly increase (`FORMAT2_EORDER`, section 5.4); **then** a
   WRITE's data is checked against its carried hash (`FORMAT2_EDATA`, `DataMismatch`), and a COPY's source --
   read through a caller-supplied `format2_read_active_fn` -- is checked against its carried hash
   (`FORMAT2_ESOURCE`/`FORMAT2_ESOURCE_UNREADABLE`, `SourceMismatch`/`SourceUnreadable`). `format2_chunk_hash()`
   implements SPEC.md section 2's definition (first 8 bytes of SHA-256, no domain separation), matching the
   Rust reference's `ChunkHash::of` (see "Checked against the Rust reference implementation" below) -- the
   caller's `fn` callback only ever sees a step whose hash has already checked out.
3. `format2_manifest_begin()` / `format2_manifest_walk()`: the unsigned manifest header and its tagged
   chunk entries (HASH / BLANK / UNREADABLE). No hash checking here: the manifest is unsigned and untrusted by
   design (SPEC.md 7.2 -- "a wrong one only wastes a transfer, because every COPY and WRITE is checked on the
   device"), so there is nothing to check it against.

## What it deliberately does not check

**Ed25519.** SPEC.md's scheme 1 is a real, well-formed value this reader recognizes -- and then refuses with
`FORMAT2_ESCHEME_UNSUPPORTED`, not `FORMAT2_OK`. There is no Ed25519 implementation anywhere in this tree
(`lib/sha256.h` is the only primitive available); treating an unverifiable signature as "fine" would be
worse than refusing it outright. Only scheme 2 (unsigned, SHA-256 over the sealed domain) is fully checked.
`tests/format2_test.c` asserts this explicitly (it would fail if a future edit turned
`FORMAT2_ESCHEME_UNSUPPORTED` into a silent pass).

**A COPY with no way to read ACTIVE.** `format2_plan_walk()` cannot check a COPY's source hash without the
actual bytes of ACTIVE's chunk, which live in flash, not in the plan buffer. A caller that cannot supply a
`format2_read_active_fn` (for example, because it only has a byte stream and no flash access) gets
`FORMAT2_ENOACTIVE` on the first COPY step, not a silently-skipped check -- the same "refuse rather than
guess" choice as the Ed25519 case. `tests/format2_test.c` exercises this against the real golden plans: with
`read_active == NULL`, the walk gets exactly as far as the ERASE and the first WRITE (both real, hash-checked
data) and then stops at the first COPY.

**Destination-blank and read-back-after-program.** SPEC.md 6.2's "the destination chunk reads blank"
(`NotBlank`) and "read it back and check the hash" (`VerifyFailed`) both need DFU flash, which this decoder
never touches. Those checks belong to whoever actually calls `flash_erase_block()`/`flash_program()` from
`fn`'s callback.

## Checked against the Rust reference implementation

`format2_chunk_hash()`'s definition -- the first 8 bytes of SHA-256, with no domain separation (unlike the
descriptor seal's `"chunky image descriptor v2"` prefix) -- was confirmed, not assumed, against
[romixlab/chunky_mcu_updater](https://github.com/romixlab/chunky_mcu_updater)'s `main` branch:

- `chunky/src/hash.rs`'s `ChunkHash::of`: `Sha256::digest(data)[..8]`, the same computation, and the same
  order of operations as `Applier::run` (`chunky/src/apply.rs`) applies it in: a WRITE's data is hashed
  directly (already in the plan buffer); a COPY's source is read from ACTIVE through a flash-port trait first,
  then hashed -- the same split `format2_plan_walk()`'s `format2_read_active_fn` callback makes, for the same
  reason (the source bytes are not in the plan at all).
- Independently verified against the vendored vectors while writing this (node:crypto, not
  `format2_chunk_hash()`): `sha256(golden_unsigned_written[0:256])[:8]` equals
  `golden_unsigned_steps[1].hash`, and the same holds for the signed vector's first WRITE. `check_plan_walk_prefix()`
  in `tests/format2_test.c` checks this through the actual code, not just by inspection.
- The vendored vectors carry no ACTIVE bytes (only `golden_*_written`, the WRITE side), so a COPY's source hash
  can't be checked against real data here; `check_hash_checks()` instead uses a small synthetic plan and a mock
  ACTIVE, with hashes computed independently (node:crypto again) so a bug in `format2_chunk_hash()` itself
  would not go unnoticed by reusing it to generate its own expected answer.

## Files

| File | What |
|---|---|
| [`SPEC.md`](../SPEC.md) | The format 2 spec, vendored verbatim |
| [`rust/chunky_format/c/chunky_format.h`](../rust/chunky_format/c/chunky_format.h) | Generated C99 reader, vendored verbatim, no allocation, header-only |
| [`rust/chunky_format/c/golden.h`](../rust/chunky_format/c/golden.h) | Golden byte vectors (signed + unsigned plan/manifest), vendored verbatim |
| [`rust/chunky_format/c/test.c`](../rust/chunky_format/c/test.c) | Upstream's own vector test, vendored verbatim (parity check: confirms the vendored header/vectors pair is exactly what upstream ships) |
| [`rust/chunky_format/c/read_plan.c`](../rust/chunky_format/c/read_plan.c) | Upstream's example: a C peer reading any format-2 plan file |
| [`lib/format2.h`](../lib/format2.h) / [`lib/format2.c`](../lib/format2.c) | This integration: `format2_plan_begin/_walk`, `format2_manifest_begin/_walk` |
| [`tests/format2_test.c`](../tests/format2_test.c) | Golden vectors (plan, manifest, steps, entries incl. real WRITE hash checks) + refusal-stage negatives + step order + chunk-hash verification (WRITE/COPY, synthetic + mocked ACTIVE) + range/data-length cases |
| [`tests/case_format2.sh`](../tests/case_format2.sh) | Builds and runs the above; picked up automatically by `tests/run_all.sh`'s `case_*.sh` glob |

## Building it

Nothing here needs the Makefile or a Rust toolchain -- `chunky_format.h` is self-contained C99.

```sh
tests/run_all.sh case_format2.sh      # as part of the normal test glob
# or directly:
tests/case_format2.sh
# or by hand:
cc -std=c99 -Wall -Wextra -Werror -pedantic -Wconversion -Wsign-conversion \
   -iquote rust/chunky_format/c -iquote lib \
   -o build/format2_test tests/format2_test.c lib/format2.c lib/sha256.c
build/format2_test

# upstream's own reader/tests, for a from-scratch parity check:
cc -std=c99 -Wall -Wextra -Werror -pedantic -Wconversion -Wsign-conversion \
   -o /tmp/t rust/chunky_format/c/test.c && /tmp/t
```

## If this grows into real wire usage later

The natural next boundary is wherever raw bytes first arrive -- a new transport/session path that speaks
format 2, or (for the bootloader side) a staged plan read back from flash -- calling `format2_plan_begin()`
right where `manifest_check()` is called today for the existing format, and `format2_plan_walk()`'s `fn`
callback driving `flash_erase_block()`/`flash_program()` the way `bootloader.c`'s own apply loop does (with a
real `format2_read_active_fn` reading ACTIVE, and the destination-blank/read-back checks SPEC.md 6.2 still
needs added at that point, see above). That is a deliberate next step, not implied by anything added here: it
needs an Ed25519 verify first, and a decision on how (or whether) format 2's manifest/plan shape replaces
`manifest_t`/`plan_t` rather than living beside them.
