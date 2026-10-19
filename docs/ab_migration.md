# Moving the PoC from single-partition to A/B

As of 2026-10-19. Migration plan, not implemented. No code changed by this
document.

This is not a new design: DESIGN.md §2-8 already specify A/B in full —
`active_slot`/`pending_slot`, the 4-page meta bank, the boot state machine,
trial boot, unroll, revert. The PoC (`poc/`, now the repo root) implements
the simplification in §18 instead: one partition, updated in place, no
rollback. This plan brings the code up to what §2-8 already specify, and
retires §18's trade. Starting point:
[`auth_verification.md`](auth_verification.md)'s finding that the in-place
variant has no bootloader-held veto point after a sector is written, because
there is no inactive slot to gate — that gap goes away once a real inactive
slot exists to stage into.

## What's already in place: the geometry layer needs nothing

`device/stm32f103cb/device.toml` already declares `slot_a` and `slot_b` as
two equal regions, and `tools/gen_device.py` already emits both
(`build/device/geometry.h`: `SLOT_A_BASE`, `SLOT_B_BASE`, one shared
`SLOT_SIZE`), already checked for no overlap
(`gen_device.py:150-152`: "slot_a and slot_b must be the same size").
`lib/flash.h:40-50` already asserts both slots sit on sector boundaries, in
order, without overlap, within `FLASH_SIZE`. The single-partition PoC
simply aliases one name onto one of them:

```c
// lib/flash.h:53
#define APP_BASE SLOT_A_BASE
```

Everything from here is downstream of that one line — nothing in
`device/`, `tools/gen_device.py`, or the generated `geometry.h` changes.

## What changes, layer by layer

### `lib/protocol.h` — wire structs

DESIGN.md's own wire sketch (§6) already includes the fields the current
structs lack:

| Struct | Has today | DESIGN.md §6 says | Add |
|---|---|---|---|
| `msg_hello_t` (`protocol.h:76-88`) | no slot field | device reports `active slot` | `active_slot` |
| `msg_plan_begin_t` (`protocol.h:111-117`) | no slot field | "target version, **destination slot**, step count..." | `dest_slot` |
| `msg_step_copy_t` (`protocol.h:121-125`) | `dst, src_index, hash` | "`STEP_COPY`: `dst`, **`src_slot`**, `src_index`, expected hash" | `src_slot` |

`msg_manifest_auth_t`/manifest body stay structurally the same shape; only
their *length* changes (next section).

### `lib/manifest.h`/`.c` — both slots, not one

`manifest_t.h` (`manifest.h:14-18`) is sized `[MANIFEST_ENTRIES][...]` with
`MANIFEST_ENTRIES = SECTORS_SLOT` (`flash.h:55`) — one slot's worth.
DESIGN.md §4: "The device hashes both slots and reports all of it" — 104
entries on the reference part, slot A's sectors in order then slot B's.
Needed:

- `MANIFEST_ENTRIES` becomes `2 * SECTORS_SLOT` (or `manifest_t.h` becomes
  `[2][SECTORS_SLOT]` — equivalent, pick one and keep `manifest_hash_at()`'s
  signature consistent with it).
- `manifest_build()` (`manifest.c`, called from `flash_sector_hash()`-style
  reads) hashes slot A then slot B, not just `APP_BASE`.
- `manifest_check()` (`manifest.c:32-38`) checks `sectors == 2 *
  SECTORS_SLOT`.
- `plan.c`'s `manifest_hash_at()` callers need a slot-aware accessor (index
  within `[0, 2*SECTORS_SLOT)`, or `(slot, index)` — same choice as above.

### `lib/flash.h`/`.c` — slot-aware reads/writes, and `meta_t` splits

`flash_write_app_sector()` (`flash.c:81-90`) and `flash_sector_hash()`
(`flash.c:93-107`) hardcode `APP_BASE`. Both need a slot parameter (or a
`slot_base(int slot)` helper feeding the same functions) so the session can
write the *pending* slot while reading the *active* one — never the same
offset for both, which is the one case `flash_write_app_sector()` must now
refuse or the apply invariant breaks.

`meta_t` (`flash.h:107-121`) is reset wholesale by `meta_init()`
(`flash.c:117-123`, a `memset` then three fields) at the start of every new
plan (`device.c:144`, `on_offer()`). That is fine for the fields that are
genuinely per-plan scratch (`target_version`, `target_sha256`, `op[]`,
`src_index[]`, `target_hash[][]`) — it is wrong for `active_slot`, which
must survive every plan cycle, including ones that fail. DESIGN.md §2's
own page-0 layout already separates these conceptually (`MetaHeader`:
"...`active_slot`, `pending_slot`, `plan_state`, `target_version`...") and
page 3 is a *separate* page for the attempt log, not reset with the plan
header. Needed:

- Add `active_slot`, `pending_slot` to `meta_t`, and an attempt-log
  structure (DESIGN.md §2 page 3: "16-bit tokens cleared one per trial
  boot, plus the last 8 boot outcomes").
- `meta_init()` must take (or be split into two calls: one for "new plan,"
  one for "fresh device") so it clears plan-scratch fields without
  clobbering `active_slot`/the attempt log on an existing device.
- `sizeof(meta_t)` grows; recheck the `_Static_assert` at `flash.h:130-132`
  against `JOURNAL_OFF` for the reference geometry (currently 4 KB of
  `META_HDR_UNITS`; the attempt log may need its own unit, matching
  DESIGN.md's 4-page layout, rather than growing page 0 further).
- Existing `.bin` flash-image test fixtures were created against the
  current `meta_t` layout; any fixture that embeds a meta bank (not just
  an application image) needs regenerating once the struct changes.

### `lib/plan.h`/`.c` — plan against the *destination* slot, source from *active*

`plan_build()` (`plan.c:4-53`) takes one manifest and one target, both
implicitly "the one slot." It needs: which slot is the destination (always
the current inactive slot — the server gets this from `HELLO`'s
`active_slot`), and the COPY search (`plan.c:34-37`, "the device reports
`H` anywhere — any slot, any index," DESIGN.md §5 rule 2) now searches
*both* slots' reported hashes, not one array. The in-place-only restriction
this PoC currently enforces — "a COPY source must be a sector the plan
leaves alone" (DESIGN.md §18, `tests/case_copy_src_rewritten.sh`) — is a
consequence of writing and reading the same partition; it does not apply
once sources come from the untouched active slot. Sector 0's "always last"
rule (`plan.c:47-49`, the commit-marker reasoning) still holds — it is per
descriptor, not per partition layout.

### `lib/device.c`/`.h` — the real work

This is where most of the effort lands.

**Session side (`on_offer`, `on_step`, `on_plan_end`, `device.c:124-270`):**
every call that writes `APP_BASE + offset` (`stage_sector()` at
`device.c:160-193`, the dictionary read at `device.c:259`) instead writes
`slot_base(pending_slot) + offset`, where `pending_slot` is the slot
*not* currently active — computed once at session start from the loaded
`meta_t.active_slot`, never from anything the wire sends. `msg_step_copy_t`
now carries `src_slot`; `on_step`'s `STEP_COPY` handling (`device.c:225-237`)
records it alongside `src_index` instead of assuming the session's own
partition.

**Boot side (`device_boot()`, `device.c:333-440`) needs the state machine
DESIGN.md §7 already specifies and the current code does not implement at
all** — today's flow goes `PLAN_STAGING` straight to `PLAN_COMMITTED` with
no trial, because in-place has nothing to trial against. A/B needs:

1. Apply (as today: COPY/WRITE steps against the journal), then verify the
   pending image, same as now — but into the *pending* slot, source reads
   from `active_slot` (DESIGN.md §7 step 2's `src_slot`/`src_index` against
   `pending_slot`/`dst`).
2. On verify success: **new** — set `pending_slot` to trial (write the
   attempt log with `MAX_TRIES` tokens, `plan_state = TRIAL`), start the
   (simulated) watchdog, "jump" to the pending slot.
3. **New** — a `plan_state == TRIAL` path: clear one attempt token on
   entry; none left → revert (§8); otherwise this counts as a trial run.
4. **New** — `cb_confirm()`: the equivalent of the application's "I reached
   the server, I'm healthy" call, which flips `active_slot = pending_slot`,
   `plan_state = COMMITTED`, clears the attempt log. Today's design has no
   application-confirm step at all (commit is immediate on `MSG_COMMIT` at
   `on_commit()`, `device.c:272-288`); A/B needs this as a step *after* a
   successful trial boot, not at plan-apply time.
5. **New** — revert (§8): mark the pending slot's header `REVOKED` (or
   equivalent — DESIGN.md's slot-header revocation has no PoC analogue
   yet), write `active_slot` unchanged, `plan_state = NONE`,
   `last_result = TRIAL_FAILED`, boot the (unchanged) active slot.
6. Unroll (§8, abandon before any boot attempt) is closer to what exists
   today (`device.c:345-350`'s "plan was never committed, discarding it")
   but must now erase sector 0 of the *pending* slot specifically, not
   assume there is only one slot to discard into.

**Open question this plan does not resolve:** DESIGN.md's trial/attempt
mechanism assumes a real watchdog and a real CPU reset between attempts.
The host PoC has neither. `device_boot()` is called once per test today;
simulating "the trial image hung and reset three times" needs either a
test harness that calls `device_boot()` repeatedly with the attempt log
persisting across calls (closest to the real mechanism), or a deliberately
simplified single-call trial (resolve pass/fail immediately, skip the
multi-attempt tokens). Decide this before writing `device_boot()`'s TRIAL
branch — it changes the function's calling contract.

### `middleware/` — provisioning, sessions, server-side planning

- `flash_file.c:100-110` (factory install) writes one `SLOT_SIZE` image at
  device creation. It should keep writing only slot A; slot B stays erased
  (DESIGN.md: "slot B: nobody yet," now literally true rather than
  "reserved and unused by this variant"). `active_slot` in the fresh
  device's meta bank is initialized to A here.
- `server_session.c:85` (`HELLO` geometry check) and the manifest-sized
  buffers at `server_session.c:182-196, 303-304` all key off `SECTORS_SLOT`
  — correct already (one slot's size), but the server's *planning* step
  needs `active_slot` (from `HELLO`) to know which slot is the diff
  *destination* (the other one) and which is the trusted COPY-source slot.
- `client_session.c` is a thin wrapper (39 lines) and likely needs no
  change beyond whatever `device_session_*`'s signature picks up.

### `src/` — CLIs

`cbflash.c`'s `manifest`/`find`/`map` commands (`cbflash.c:73-109`) read
one slot via `APP_BASE`; they should report both, labeled, once the
geometry they're built against has two live slots instead of one reserved
one. `server.c`'s wire-savings accounting (`server.c:49-173`) is already
per-plan/per-slot-sized and should need little beyond the manifest-size
doubling.

### Tests and scripts

- `tests/case_copy_src_rewritten.sh` tests an in-place-only restriction
  (§18) that no longer applies — retire it or repurpose it to assert the
  *opposite* (a COPY from the now-safe active slot is allowed even when
  the destination slot's own copy of that sector was already rewritten
  this plan).
- New cases needed: a trial boot that confirms; one that fails
  `MAX_TRIES` times and reverts; an unroll before any boot attempt; a COPY
  whose source is the active slot while the destination slot is mid-write
  (should succeed — the invariant protects exactly this); resume after a
  power cut mid-apply, now with two slots' manifests to re-diff against.
- `tests/run_matrix.sh`'s per-geometry rebuild should keep working
  unchanged — it already rebuilds via the `device.toml`/`gen_device.py`
  path, which needs no change here.

### Docs that go stale once this lands

- DESIGN.md §18 ("Single-partition variant") describes the PoC's current
  behavior; once A/B lands it describes a variant the PoC no longer runs.
  Mark it historical or delete it in the same change that lands A/B, not
  before — it is the only place the single-partition tradeoffs are
  recorded, and deleting it early would lose that record while the old
  behavior is still what's running.
- `docs/compare_chunky_mcu_updater.md` and `docs/auth_verification.md`
  both describe the gap ("flash written before signature checked") as
  arising *because* the PoC has no second slot. Once A/B lands, revisit
  both: the core authentication questions (Scheme A/B, meta header
  integrity) stand regardless of partition layout, but their "why this
  matters here" framing should point at trial-boot/revert instead of
  "no inactive slot to stage into."

## Suggested order

1. `protocol.h` wire structs (breaking; no deployed devices to migrate).
2. `manifest.h`/`.c`, `plan.h`/`.c` — two-slot manifest and planning, no
   device.c changes yet; provable independently with unit-level tests
   against two in-memory manifests.
3. `flash.h`/`.c` — slot-aware read/write helpers, `meta_t` restructure.
   Decide the attempt-log/simulated-reset question here (open item above)
   before step 4.
4. `device.c` session side: write to `pending_slot`, record `src_slot`.
   At this point sessions work but boot still does the old
   straight-to-committed thing — fine as an intermediate, testable state.
5. `device.c` boot side: trial boot, `cb_confirm()`, revert, unroll.
   This is the largest single step; land it with the new test cases from
   the section above, not after.
6. `middleware/`, `src/` catch-up: provisioning, HELLO/manifest field
   wiring, CLI reporting for both slots.
7. Docs: retire/archive §18, update the two docs listed above, update
   `README.md`'s PoC description (it currently says "the PoC still updates
   slot A in place; slot B is reserved").

## Open questions to settle before coding

- [x] Simulated reset/watchdog for the TRIAL attempt-token loop (above) —
  **chose the test-harness option**: one `device_boot()` call is one
  simulated boot/reset cycle, with `meta_t.attempts_left` persisting in
  flash across calls exactly as a real attempt log would across resets.
  A trial boot that succeeds does *not* auto-confirm inside `device_boot()`
  — the caller (standing in for the application) must call the new
  `device_confirm()` separately, or call `device_boot()` again to simulate
  a reset with no confirm, which consumes another attempt. This is driven
  on the real `cb-client` binary via three new test-only flags
  (`--boot-only`, `--confirm-only`, `--session-only`) plus `--no-confirm`
  on the default flow, rather than a separate C unit-test harness this repo
  doesn't otherwise have — see `src/client.c` and
  `tests/case_trial_confirm.sh`/`case_trial_revert.sh`.
- [x] `meta_t` layout: **kept one struct** with two reset entry points —
  `meta_init()` (fresh device: `active_slot = A`, no plan) and
  `meta_reset_plan()` (new plan on an existing device: clears only the
  per-plan scratch fields, leaves `active_slot`/`pending_slot`/
  `attempts_left`/`last_result` alone). The literal page-0/page-3 split
  would be more faithful to DESIGN.md §2's physical layout, but there is no
  behavioural reason to pay for it here: every `meta_store()` already
  erases and rewrites the whole bank (the ping-pong), so `attempts_left`
  has no incremental-reprogram hazard forcing it into a page of its own the
  way the journal (never erased while a plan is live) needs its
  clear-only encoding. Chose the smaller diff; see `lib/flash.h`'s comment
  on `meta_t` for the full reasoning.
- [x] Slot-header revocation (`REVOKED` marker, DESIGN.md §8 revert step 1)
  — added `image_desc_t.revoked` (`lib/image.h`): `IMG_NOT_REVOKED`
  (`0xFFFFFFFF`, the erased value) or `IMG_REVOKED` (`0x00000000`), sitting
  outside both `image_sha256`'s domain and the MAC's signed prefix, so
  revert can program it in place (bits only clear, same discipline as every
  other live-written field here) without touching either hash.
  `image_verify_flash()` refuses a revoked slot; `image_revoke_flash()` sets
  it.
- [x] Flat `[0, 2*SECTORS_SLOT)` manifest index, slot A first — chosen
  because it is already what MANIFEST's wire body is (no slot field per
  entry). `manifest_index()`/`manifest_slot_of()`/`manifest_sector_of()`
  (`lib/manifest.h`) are the only places that convert between it and
  `(slot, sector)`. The wire step/plan structs (`msg_step_copy_t`,
  `step_t`) carry an explicit `src_slot` field instead, per the table in
  "`lib/protocol.h` — wire structs" above — the two choices don't need to
  match since nothing converts directly between them in a way that would
  create an inconsistency.
- [x] Existing `.bin` test fixtures: none of this repo's test fixtures
  embed a pre-built meta bank (`tests/*.sh` builds `flash.bin` fresh, via
  `flash_file_create()`/`device_open_or_create()`, in every case), so
  `meta_t`'s layout change needed no fixture regeneration.
