# Authentication: open cases

As of 2026-10-20. Known gaps and limits left after implementing
[`auth_implementation_plan.md`](auth_implementation_plan.md). The design
behind them is in [`auth_verification.md`](auth_verification.md).

## Risks

### The bootloader does not repeat the downgrade check

The updater rejects a lower `version` at `PLAN_BEGIN` unless the descriptor
sets `allow_downgrade`. The bootloader's `plan_check()` authenticates the
pending descriptor and its `table_root`, but does not compare the pending
version with the active one. A party that holds `dev_key` can forge a valid
`meta_t` HMAC and stage an older, correctly signed image, which the
bootloader then trial-boots.

Fix: in `plan_check()`, reject a pending `version` lower than the active
slot's unless the pending descriptor sets `allow_downgrade`. This is a few
lines and no new state.

### `revert_plan()` trusts `pending_slot` from meta

`unroll_plan()` erases only when `pending_slot` names the inactive slot.
`revert_plan()` has no such guard: it marks whatever slot `meta_t` names as
`REVOKED`. A forged meta (again, `dev_key` holder) with `plan_state = TRIAL`,
`attempts_left = 0` and `pending_slot = active_slot` would revoke the
running image. The fallback path then boots the other slot if it verifies;
otherwise the device has no bootable image.

Fix: apply the same `pending_slot == OTHER_SLOT(active_slot)` guard in
`revert_plan()`, and treat a mismatch as a corrupt plan (drop it, keep the
active slot).

### A descriptor sector that fails to decompress ends the plan

The descriptor sector is the commit marker and must be the last step. The
updater marks it seen even when `STEP_WRITE_END` NAKs it with
`NAK_DECOMPRESS` (`lib/device.c`, `on_step()`), so the server cannot resend
it in the same session; `PLAN_END` then fails the staged-descriptor check.
The outcome is safe (nothing boots), but one corrupt frame on the last
sector costs the whole session.

Fix if it matters on lossy links: set `desc_seen` only after
`stage_sector()` succeeds for the descriptor sector, so a NAK allows a
resend.

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
