# Authentication implementation plan

As of 2026-10-20. Implementation plan for the schemes proposed in
[`auth_verification.md`](auth_verification.md), on top of the A/B layout
(DESIGN.md sections 2-8). Read that doc and DESIGN.md sections 6-8 and 10
first.

## Starting point

- The A/B gap is closed: an unauthenticated session can fill the inactive
  slot, but cannot make it bootable. Scheme A (per-chunk authentication) is
  an early-reject optimization; Scheme B (the bootloader's whole-image
  check) stays the security boundary.
- `image_len` is always `SLOT_SIZE` today (`lib/image.c:36`, enforced at
  `lib/flash_view.c:57`). The image is padded with `0xFF` to the slot end,
  so a smaller firmware still sends WRITE steps for the padded tail and the
  hash covers every sector of the slot.
- The image signature already travels inside the image: `image_desc_t` sits
  at `DESC_OFF = 0x200`, in sector `DESC_SECTOR = DESC_OFF / SECTOR_SIZE`
  (sector 0 at 1 KB sectors). The version changes every release, so that
  sector is always a WRITE step, never a COPY.
- The table of target per-sector hashes depends only on the build, not on
  the device or the diff. It can be signed once at build time by the image
  signer; the server needs no online signing key for it.

## Variable chunk count (bigger or smaller firmware)

1. `image_len` becomes the real image size, rounded up to `SECTOR_SIZE`.
   `N = image_len / SECTOR_SIZE` is the number of sectors the image uses.
2. The image hash covers `[0, image_len)` minus the descriptor hole
   `[DESC_OFF, CODE_OFF)`, instead of `[0, SLOT_SIZE)`. `image_hash()`,
   `image_hash_flash()` and `image_build()` change to match.
3. Validation, both at `PLAN_BEGIN` and in `image_verify_flash()`:
   `CODE_OFF <= image_len <= SLOT_SIZE` and `image_len % SECTOR_SIZE == 0`.
4. `lib/plan.c` diffs only `[0, N_new)`.
   - Bigger firmware: more sectors, so more steps.
   - Smaller firmware: fewer steps. Sectors `[N_new, SECTORS_SLOT)` are
     outside the hash domain: no step, no erase. The bootloader never reads
     them. Stale old code stays there as unsigned dead bytes. **Decision:**
     the bootloader does not erase the tail (avoids wear; revisit if needed).
5. The sector hash table is dense: one entry for every sector in `[0, N)`,
   including unchanged ones. Position is implied by the index, so no `dst`
   field is needed in the table. `N` comes from the signed `image_len`, so a
   server cannot truncate the table. `step_count <= N`.

## Where the signatures go

Two signatures, two jobs.

- **Table authentication (Scheme A) comes first**, before any step. Arriving
  last would make per-chunk checking impossible and gain nothing over today.
  - `image_desc_t` gains `table_root[32] = SHA256(hash_0 || ... ||
    hash_{N-1})` over the `SECTOR_HASH_LEN`-truncated sector hashes. It fits
    in the 44 reserved bytes.
  - `image_mac` is extended to cover `table_root`. One build-time signature
    then authenticates both the full image hash and the table.
- **The whole-image signature (Scheme B) arrives in the last chunk.** It lives
  in the descriptor sector, and that sector is sent as the **last WRITE
  step**.
  - Until the last chunk lands, the pending slot has no valid header, so an
    interrupted transfer always leaves an invalid slot. This matches
    `unroll_plan()`, which erases sector 0 for the same reason.
  - The device enforces it: once `DESC_SECTOR` is staged, only `PLAN_END` is
    accepted.
  - Other steps may arrive in any order: each is checked by lookup in the
    already-authenticated table.
- `PLAN_BEGIN` also carries the descriptor's signed fields and `mac`, so the
  updater verifies them before the first chunk. At `PLAN_END` the updater
  checks that the staged descriptor bytes equal those fields.
- **Decision:** Scheme B keeps the raw full-width `image_sha256` (no fold).
  `table_root` covers Scheme A; the boot-time check stays a 32 B compare.

## Steps

Each step ends with `make` and `tests/run_all.sh` passing (see the `build`
skill, `.claude/skills/build/SKILL.md`).

### Step 1. Split the bootloader into `lib/bootloader.c` (no behavior change)

**Done, 2026-10-20.** As written; `ECDSA_SIGN_MS`/`ECDSA_VERIFY_MS` moved to
`lib/stats.h` with `device_stats_t` and `stats_tick()`. Verified a pure move: the
suite's wire byte counts were identical before and after.

- Move `device_boot()` (renamed `bootloader_boot()`), `unroll_plan()`,
  `revert_plan()` and `boot_flash_fail()` from `lib/device.c` into
  `lib/bootloader.c`, with `lib/bootloader.h`.
- `device_confirm()` stays in `device.c`: confirming a trial boot is the
  application's duty (DESIGN.md section 7).
- `device_app_version()` is used by both: move it to `lib/flash_view.c` as
  `image_version_flash()` (declared in `image.h`).
- Move `tick()` and `device_stats_t` into a small shared header (e.g.
  `lib/stats.h`) so `bootloader.c` does not include `device.h`.
- `bootloader.c` includes only `flash.h`, `image.h`, `manifest.h`
  (`sector_hash`), `sha256.h`, `util.h` and the stats header. No session or
  protocol code.
- Update `LIB_SRC` in the `Makefile`, `src/client.c` callers, and comments
  that name `device_boot()`.

### Step 2. Variable `image_len`

**Done, 2026-10-20.** As written, plus: `image_hash_flash(f, slot, image_len,
out)` takes the length (the caller reads it from the descriptor) and returns -1
for a length that is not `image_len_ok()`; `image_build()` returns the length
through a pointer; `msg_plan_begin_t` gained `image_len`; `meta_t` records
`image_len` and the bootloader refuses a descriptor whose length differs from
it; `device_stats_t.verify_bytes` replaces the fixed `SLOT_SIZE` in the verify
timing model. `DESC_SECTOR` is sector 0 in every supported geometry
(`CODE_OFF <= SECTOR_SIZE` is a static assert), so there is no geometry where
the descriptor sector differs. Test: `tests/case_variable_len.sh`.


- `lib/image.c`, `lib/flash_view.c`, `lib/plan.c` as described above.
- `meta_t` records `N` (or `image_len`) for the pending plan.
- Server and CLIs build images with the real length.
- Tests: update to a bigger image, then to a smaller one and back; check
  step counts shrink/grow and each result verifies and boots.

### Step 3. Descriptor `table_root` and extended MAC

**Done, adjusted, 2026-10-20.** `table_root` sits right after `sha256` so the
signed prefix stays contiguous (`[0, 0x50)`, `mac` and `revoked` follow, 12 B
reserved); `IMG_MAGIC` is now `CBI2`. **Adjustment:** `table_root` lives in the
descriptor sector, so that sector's table entry cannot cover the descriptor
(the hash would depend on its own result). Its entry is taken over the sector
without the 128 descriptor bytes (`image_sector_hash()`, `image_sector_hash_flash()`),
the same hole `image_sha256` has; the descriptor itself is checked whole against
`PLAN_BEGIN` (step 4). Consequences: `stage_sector()` compares raw hashes for
its "already in flash" skip and its post-program check (the hole hash cannot
tell a stale descriptor from a new one), and the planner never makes the
descriptor sector a COPY.


- Add `table_root[32]` to `image_desc_t` (from `reserved`).
- `image_build()` computes it; `image_mac` covers the signed prefix plus
  `table_root`. `revoked` stays outside the MAC.
- Bump `IMG_MAGIC`.

### Step 4. Updater checks (part 1, `lib/device.c`)

**Done, adjusted, 2026-10-20.** `PLAN_BEGIN` (112 B) carries `image_len`,
`flags`, `image_sha256`, `table_root` and `image_mac`; the device rebuilds the
whole descriptor from them (everything else is fixed by the format) and
compares it with flash at `PLAN_END`, covering a descriptor sector the planner
skipped as already in place too. The table is received straight into the
plan's dense `meta_t.target_hash`. `PLAN_TABLE_END` has no payload. The
version check is `version >= running` unless the signed `allow_downgrade` flag
is set. Beyond the plan: a `STEP_COPY` for the descriptor sector is refused;
`PLAN_END` also checks that the staged descriptor sector hashes (without the
descriptor) to the table's entry; steps no longer carry a hash.


1. Thread `root_key` into `device_session_start()`.
2. `PLAN_BEGIN` carries the descriptor's signed fields plus `mac`. Verify the
   MAC, version/downgrade rule and `image_len` bounds; reject before any
   flash write.
3. New messages `PLAN_TABLE` (fragments, shaped like `MANIFEST`) and
   `PLAN_TABLE_END`. Table is `N * SECTOR_HASH_LEN` bytes (~816 B for 102
   sectors at 8 B). Check `SHA256(table) == table_root`, then set
   `table_ok`. Must fit `FRAME_MAX_PAYLOAD` (DESIGN.md section 6).
4. `stage_sector()` and the `STEP_COPY` handler check against `table[dst]`,
   not the step's own hash (drop the hash field from the step messages).
   Any step before `table_ok`, or with `dst >= N`, is rejected.
5. `DESC_SECTOR` must be the last WRITE. At `PLAN_END`, compare the staged
   descriptor with the `PLAN_BEGIN` fields.
6. Server side (`middleware/server_session.c`): send the table after
   `PLAN_BEGIN`, order the descriptor sector last.
7. Update DESIGN.md section 6 for the new messages and field changes.

### Step 5. Bootloader checks (part 2, `lib/bootloader.c`)

**Done, 2026-10-20.** `plan_check()` runs before any COPY: pending slot is the
inactive one, descriptor magic, `image_len` equal to the plan's, signature,
`sha256` equal to the plan's, and the table fold against `table_root`. The
plan's `OP_COPY` for the descriptor sector is refused. `unroll_plan()` now
erases only the inactive slot, so a forged `pending_slot` cannot erase the
active one.

**Follow-up, 2026-10-20** (see [`auth_open_cases.md`](auth_open_cases.md)):
`plan_check()` also refuses a downgrade; the full pending-image verify is
`pending_verify()`, run at the `STAGING` to `TRIAL` transition and on every
later trial boot; `revert_plan()` revokes only the inactive slot.


1. Before applying anything: read the pending descriptor, verify its MAC,
   and check `SHA256(meta.target_hash[0..N)) == desc.table_root`. The
   bootloader never trusts the meta plan on its own and never runs COPY
   steps from a forged meta. On failure: unroll.
2. Apply copies; check each sector against the table (as today).
3. `image_hash_flash()` over `[0, image_len)` compared at full 32 B with
   `desc.sha256`; MAC; not revoked; then trial boot.
4. Normal boots keep the cheap recompute-and-compare.

### Step 6. Meta integrity

**Done, 2026-10-20.** `meta_t.crc` is now `mac[32]`, keyed from the identity
page's `dev_key` on every load and store (`meta_store` fails if the identity
page is blank); `META_MAGIC` is "MET2".


Replace `meta_t`'s CRC-32 (`lib/flash.c:132-156`) with an HMAC under
`dev_key` over the same range. Bump the `meta_t` magic. The
bootloader/application key split stays accepted risk (DESIGN.md section 10).

### Step 7. Tests

**Done, 2026-10-20.** `tests/auth_test.c` (built into `build/auth_test` by
`make`, run by `tests/case_auth.sh`) drives the session and the bootloader
message by message. Cases: honest control, bad signature, swapped
`table_root`, table not matching `table_root`, step before the table, step not
in the table, descriptor sector not last, descriptor edited inside its hole,
`image_len` and downgrade bounds, meta bank edited without the key, meta plan
with a genuine MAC but a table that is not the descriptor's. Variable-size
updates: `tests/case_variable_len.sh`. The "meta tampered with a valid CRC
before step 6" half was not run against the old scheme.


- Bad `table_root` or bad MAC at `PLAN_BEGIN`/`PLAN_TABLE_END`: rejected
  before any `stage_sector()` call; flash unchanged.
- A step whose data hash is self-consistent but not in the table: rejected.
- Descriptor sector sent before another step: rejected.
- Meta bank tampered with a valid CRC (before step 6) / without the key
  (after step 6): caught.
- Bigger and smaller images both update, verify and boot.
- Bootloader refuses a meta plan whose table does not match the pending
  descriptor's `table_root`.

## Out of scope

- Erasing the stale tail after a shrink.
- The full-width fold for Scheme B.
- The bootloader-only key for `meta_t`'s security-relevant fields.
