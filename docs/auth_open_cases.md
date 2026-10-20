# Authentication: open cases

As of 2026-10-20. Known gaps and limits left after implementing
[`auth_implementation_plan.md`](auth_implementation_plan.md). The design
behind them is in [`auth_verification.md`](auth_verification.md).

## Risks

None open beyond the accepted limits below.

## Fixed

Found after the first implementation and fixed with a regression case each
in `tests/auth_test.c` (each case fails with the fix reverted).

- **The bootloader did not repeat the downgrade check.** A `dev_key` holder
  could forge meta to stage an older, genuinely signed image. `plan_check()`
  now rejects a pending `version` lower than the active slot's unless the
  pending descriptor sets `allow_downgrade`. Test: `t_forged_downgrade`.
- **`revert_plan()` trusted `pending_slot` from meta.** A forged trial plan
  naming the active slot would revoke the running image. `revert_plan()` now
  revokes only the inactive slot and otherwise just drops the plan. Test:
  `t_revert_active`.
- **A NAKed descriptor sector ended the plan.** The updater marked the
  descriptor sector seen even when it was NAKed, so it could not be resent
  in the same session. `desc_seen` is now set only once the sector is
  staged. Test: `t_descriptor_resend`.
- **A forged `PLAN_TRIAL` booted the pending slot unverified.** The trial
  path jumped to `pending_slot` on every `PLAN_TRIAL` boot without
  re-checking it; verification ran only on the `STAGING` to `TRIAL`
  transition. A `dev_key` holder could forge a meta straight into `TRIAL`
  and trial-boot an older signed image (a rollback past the downgrade
  check) or unsigned bytes. The full pending-image check is now one
  function, `pending_verify()` (inactive slot, `image_len`, `image_sha256`
  against the plan, MAC, not revoked, no downgrade). It runs at the
  transition and again on every trial boot that did not just pass it in the
  same call; a failure unrolls the plan. Cost: one image hash plus one
  signature verify per trial boot, at most `MAX_TRIES` per update. The
  signature is needed: the hash alone is compared against a meta value a
  forger controls. Measured on the PoC's F103 model: a trial boot went from
  107–215 ms to 417–524 ms, of which about 309 ms is the verify. Test:
  `t_forged_trial`.

### Where the fixes are

| Fix | Code | Test |
|---|---|---|
| Downgrade check in the bootloader | `lib/bootloader.c`, `plan_check()` | `t_forged_downgrade` |
| Revert revokes only the inactive slot | `lib/bootloader.c`, `revert_plan()` | `t_revert_active` |
| Descriptor sector closes the plan only once staged | `lib/device.c`, `on_step()` | `t_descriptor_resend` |
| Pending slot verified on every trial boot | `lib/bootloader.c`, `pending_verify()` | `t_forged_trial` |

The three tests drive the forged states directly:

- `t_forged_downgrade`: an honest v2 update is booted and confirmed, so slot
  A still holds the signed v1. A meta forged with the genuine `dev_key`
  stages slot A (v1) as the pending plan. The bootloader must refuse it,
  never reach a trial boot, and stay on slot B.
- `t_revert_active`: a fresh device with a forged meta of `plan_state =
  TRIAL`, `attempts_left = 0` and `pending_slot = active_slot`. The boot
  must not revoke anything, the active slot must still verify, and the plan
  must be dropped.
- `t_descriptor_resend`: the descriptor sector is first sent with a flipped
  byte (NAKed for a hash mismatch), then resent intact. `PLAN_END` and
  `COMMIT` must then succeed.
- `t_forged_trial`: two forged `TRIAL` metas. First on a fresh device whose
  slot B holds no image; then after an honest v2 update, pointing at slot A
  (the signed v1). Neither may reach a trial boot; the plan is dropped and
  the device stays on its active slot.

### Verification (2026-10-20)

- `make` and `tests/run_all.sh`: 15 passed, 0 failed. `auth_test` runs 15
  cases.
- `tests/run_matrix.sh`: 15 passed, 0 failed at each geometry
  (`ERASE_SIZE:SECTOR_SIZE` 1024:1024, 1024:2048, 2048:2048, 1024:4096, and
  1024:1024 with `SECTOR_HASH_LEN=32`).
- The tests catch the bugs: with `lib/bootloader.c` and `lib/device.c`
  reverted to the previous commit, `tests/case_auth.sh` fails the new cases
  and passes the rest. With only `lib/bootloader.c` reverted, it fails
  `t_forged_downgrade`, `t_revert_active` and `t_forged_trial` (`3 of 15`).

## Decisions pending

Two review comments, analyzed 2026-10-20 against the code at `b48fbe4`.
Neither is implemented; each needs a decision first.

### Sector hash: fixed 8 B per format version

> Review comment: "Sector hash: fixed 8 B (SHA-256/64) per format version.
> One encoding per value, nothing to negotiate in HELLO. The full image
> SHA-256 under the signature decides what boots, so the chunk hash only
> says 'already there or not'. If we ever need more, it's a new format
> version."

**The conclusion holds; the rationale is out of date.** Since `table_root`
landed, the per-sector hashes do more than say "already there or not". They
are signed (through `table_root` in the descriptor), they are the updater's
per-chunk check in `stage_sector()`, and they are the bootloader's check on
every COPY source and applied sector. 8 B is still enough, for a different
reason:

- Forging a chunk is a second-preimage attack, not a collision. The signer
  fixes the table before any chunk is sent, so a server without the release
  key must hit one specific truncated hash at one specific index: about
  2^64 work. The table is indexed by position, so there is no multi-target
  shortcut.
- The collision bound (2^32) only helps a signer, who can sign any image
  anyway.
- The full 32 B `image_sha256` under the signature still decides what boots.
  A forged chunk that matched would still fail there: a failed update, never
  a corrupt boot.

Suggested rationale: "authenticates each chunk early, through the signed
table; 8 B is enough because forging needs a 2^64 second preimage, and the
full image hash still gates boot."

**"Nothing to negotiate" is right, but keep one byte in HELLO.** Today
`msg_hello_t.sector_hash_len` is the only guard against a mismatched device
and server misparsing each other; HELLO has no other version field. Removing
it leaves "a new format version" with nowhere to go. Rename the byte to
`format_version` instead: HELLO keeps its size and the server still refuses a
mismatch.

**A format version covers more than the wire.** The width is also built
into `table_root` in the signed descriptor (bump `IMG_MAGIC`) and into the
layout of `meta_t.target_hash[]` (bump `META_MAGIC`). One format version
should move all three together.

**If adopted:**

- `SECTOR_HASH_LEN` stays as a fixed constant 8; most uses are unchanged.
- Remove the `CB_SECTOR_HASH_LEN` override and its `_Static_assert` range in
  `lib/protocol.h`, the `SECTOR_HASH_LEN` variable in the `Makefile`, and
  the 32 B config in `tests/run_matrix.sh`.
- Rename the HELLO field in `lib/protocol.h`, `lib/device.c` and
  `middleware/server_session.c`; set `format_version = 1`.
- Update DESIGN.md section 4 ("build-time choice from 8 to 32"), sections 6
  and 10.2, and the comment in `lib/protocol.h` that says to widen the hash
  "if a per-sector hash ever becomes a trust anchor".

Tradeoff: no wider hash without a new format version, in exchange for one
fixed encoding, a smaller test matrix, and no server/device build mismatch
to diagnose.

### Manifest unsigned in layer 0

> Review comment: "Manifest: unsigned in layer 0. Device identity and a
> signed manifest with a nonce can be an optional layer on top, for
> deployments that need it, so small parts don't pay ~3 KB of flash and a
> provisioning step."

**The boot-security argument holds.** The signed manifest gives the server
authenticated device identity and a fresh, true view of the device's flash,
so the server builds the right diff. It does not decide what boots: that is
the release signature, verified with `root_key`, a public key that needs no
per-device secret. A lying manifest produces a wrong diff, which fails on
arrival (`table_root`) or at verify (`image_sha256`): a failed update, never
a corrupt boot.

**What layer 0 gains:** no identity page and no provisioning step; no
on-device signing code (in production, ECDSA signing plus a nonce source;
ECDSA verify stays, for releases); `CHALLENGE` can go, saving a round trip.
The manifest elision (DESIGN.md section 17.1) is unaffected, because
`manifest_digest` is already keyless.

**What layer 0 gives up:**

- `device_id` in HELLO is an unauthenticated claim: fine for routing and
  caching, not for deciding who may receive which image.
- Anyone can pose as a device and download images. Firmware confidentiality,
  if needed, belongs to the transport (DESIGN.md section 10.3).

**Gap 1: the meta MAC needs `dev_key`.** `meta_t` is authenticated with an
HMAC under `dev_key` (`lib/flash.c:148`). Layer 0 has no `dev_key`, so it
falls back to CRC-32, which catches accidental corruption only.

- Covered without the key: the bootloader re-checks every plan and every
  trial boot against the signed descriptor and `table_root`, so a forged meta
  cannot make it apply or run an unverified or older pending image.
- Still open to someone who can write flash directly: denial of service
  (unroll or revoke the inactive slot, reset trial counters), and a rollback
  by flipping `active_slot` to the older signed image in the other slot. An
  ordinary boot checks that slot's hash and signature, not its version. With
  `dev_key` that flip needs the key, but a compromised application holds the
  key anyway, so the HMAC never stopped that attacker.

**Gap 2: rollback protection needs a version floor.** Only a minimum version
stored where the application cannot write (option bytes, OTP, or a
bootloader-protected page) blocks the `active_slot` flip. chunky_mcu_updater
has such an anti-rollback floor. It is independent of manifest signing, so it
belongs in layer 0, not in the optional layer.

**Suggested rewording of the comment:**

> Manifest: unsigned in layer 0; it only steers the diff, and the release
> signature plus `table_root` decide what is written and what boots. Device
> identity, a nonce-signed manifest and a keyed meta MAC form an optional
> layer for deployments that need authenticated device state, avoiding
> ~3 KB of flash and a provisioning step on small parts. Layer 0 meta
> integrity is CRC-32; rollback protection comes from a version floor the
> application cannot write, not from meta.

**If adopted:**

- A build flag (for example `CB_DEVICE_IDENTITY`) switching the
  CHALLENGE/MAC path in `lib/device.c` and `middleware/server_session.c`,
  `meta_t`'s MAC versus CRC-32 (`lib/flash.c`), and the identity page
  (`device/*/device.toml`, `middleware/flash_file.c`).
- Tests in both modes.
- Separately: a version floor checked in `lib/bootloader.c` (`final_boot`
  and `pending_verify()`).

If layer 0 is adopted, the "Compromised application" limit below applies
only to the optional layer.

## Accepted limits

### Compromised application

`meta_t` is authenticated with `dev_key`, which the application holds. This
closes direct flash tampering without the key, not a compromised
application, which can forge any meta. A bootloader-only key for the
security-relevant meta fields remains out of scope, consistent with
DESIGN.md section 10's scoping.

### Stale tail after a smaller image

When a new image is smaller, sectors `[N_new, SECTORS_SLOT)` of the slot keep
the old bytes. They are outside the hash domain and are never read by the
bootloader, but old code stays in flash. Erasing the tail during apply would
remove it at the cost of extra erase cycles.

## Not done

- **CRC tamper test before the fix.** The plan asked for a test that shows a
  tampered meta with a valid CRC-32 passing before the HMAC change. The CRC
  was replaced in the same series, so only the after-fix test exists.
- **Ascending `dst` enforcement.** Not implemented, by decision: the table is
  dense and indexed by position, so step order does not affect
  authentication. Only the "descriptor sector last" rule is enforced.
- **Double HMAC at boot.** The bootloader computes the descriptor HMAC once
  in `plan_check()` and again at final verify. Harmless on the host; on a
  real part with ECDSA it is a second signature verify per update and is
  worth passing through.
