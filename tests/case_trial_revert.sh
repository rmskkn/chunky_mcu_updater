#!/usr/bin/env bash
# A/B revert after MAX_TRIES failed trial boots (DESIGN.md section 8): a
# trial image that never confirms costs one attempt token per simulated
# reset, driven here with repeated cb-client --boot-only calls (no
# --confirm-only in between), exactly docs/ab_migration.md's resolution
# for the no-real-watchdog PoC. After MAX_TRIES (3) trial boots with no
# confirm, the next boot call finds no attempts left and reverts: the
# pending slot's image is marked revoked, active_slot never moved, and the
# device is left bootable on exactly what it was running before the
# update.
source "$(dirname "$0")/common.sh"
need_build
test_init trial_revert

gen_random_bin "$BASE" $((PAYLOAD_MAX * 5 / 10)) 202
cp "$BASE" "$IMAGE"
patch_sector "$IMAGE" $((SECTORS * 2 / 10)) 0x88

trap server_stop EXIT
server_start

out="$("$CB_CLIENT" --flash "$FLASH" --initial "$BASE" --port "$PORT" --no-confirm 2>&1)"
assert_contains "$out" "attempt 1/3" "first trial boot consumes attempt 1 of 3"

# Two more resets with no confirm: attempts 2 and 3.
out2="$("$CB_CLIENT" --flash "$FLASH" --boot-only 2>&1)"
assert_contains "$out2" "attempt 2/3" "second reset without confirm consumes attempt 2 of 3"
out3="$("$CB_CLIENT" --flash "$FLASH" --boot-only 2>&1)"
assert_contains "$out3" "attempt 3/3" "third reset without confirm consumes attempt 3 of 3"

# A fourth reset finds no attempts left and reverts.
out4="$("$CB_CLIENT" --flash "$FLASH" --boot-only 2>&1)"
assert_contains "$out4" "reverting after 3 failed trial boots" "the fourth reset reverts"
assert_contains "$out4" "slot B revoked" "the pending slot is marked revoked"
assert_contains "$out4" "staying on slot A" "active_slot never moved: still booting what ran before"
assert_contains "$out4" "booting version 1" "falls back to the original, unchanged image"

status="$("$CB_CLIENT" --flash "$FLASH" --status 2>&1)"
assert_contains "$status" "state NONE" "plan_state is NONE after revert, no plan in flight"
assert_contains "$status" "active slot A" "active_slot is still A"
assert_contains "$status" "last_result 4" "RES_TRIAL_FAILED reported in the device's own state"
assert_contains "$status" "revoked after a failed trial boot" "slot B's descriptor is revoked and image_verify_flash() says so"

# Confirming now (nothing to confirm) must not crash or silently succeed
# as if a trial were still live.
confirm_out="$("$CB_CLIENT" --flash "$FLASH" --confirm-only 2>&1)"
status2="$("$CB_CLIENT" --flash "$FLASH" --status 2>&1)"
assert_contains "$status2" "active slot A" "a stray confirm after revert changes nothing"

ok "trial_revert"
