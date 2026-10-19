#!/usr/bin/env bash
# A/B trial boot that succeeds: after a plan applies and verifies, the
# bootloader boots the pending slot on trial (DESIGN.md section 7 steps
# 4-5) rather than committing immediately. The application (here, the test
# itself, standing in for it) then calls cb_confirm() -- device_confirm(),
# cb-client's --confirm-only -- exactly once, which promotes active_slot
# and clears the attempt log. docs/ab_migration.md's resolution for
# simulating repeated trial attempts without a real watchdog: one
# device_boot() call per simulated reset, driven here through cb-client
# --boot-only / --confirm-only on the real binary.
source "$(dirname "$0")/common.sh"
need_build
test_init trial_confirm

gen_random_bin "$BASE" $((PAYLOAD_MAX * 5 / 10)) 201
cp "$BASE" "$IMAGE"
patch_sector "$IMAGE" $((SECTORS * 2 / 10)) 0x77

trap server_stop EXIT
server_start

# Session + boot, but withhold the auto-confirm so the trial is observed
# mid-flight.
out="$("$CB_CLIENT" --flash "$FLASH" --initial "$BASE" --port "$PORT" --no-confirm 2>&1)"
echo "$out" | grep -E 'plan v|booting|trial'
wire_stats "$out"
assert_contains "$out" "trial boot" "the first boot after a successful apply is a trial, not an immediate commit"
assert_contains "$out" "attempt 1/3" "one attempt token spent on the first trial boot"

status="$("$CB_CLIENT" --flash "$FLASH" --status 2>&1)"
assert_contains "$status" "state TRIAL" "device is left in TRIAL, waiting on confirm"
assert_contains "$status" "active slot A" "active_slot has not moved yet"
assert_contains "$status" "pending slot B" "pending_slot B is the trial slot"

# The application reaches a working state and confirms.
confirm_out="$("$CB_CLIENT" --flash "$FLASH" --confirm-only 2>&1)"
assert_contains "$confirm_out" "confirming slot B" "device_confirm() promotes the trial slot"

status="$("$CB_CLIENT" --flash "$FLASH" --status 2>&1)"
assert_contains "$status" "state COMMITTED" "plan_state is COMMITTED after confirm"
assert_contains "$status" "active slot B" "active_slot promoted to the confirmed slot"
assert_contains "$status" "slot B          image     version 2  verified" "slot B is the verified v2 image"

ok "trial_confirm (rx=$WIRE_RX B)"
