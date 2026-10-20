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
