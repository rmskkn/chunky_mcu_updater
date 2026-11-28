#!/usr/bin/env bash
# End-to-end format2 session (SPEC.md; MIGRATION_FORMAT2.md): provision, update over a real cb-server/cb-client TCP session, apply/verify/trial-boot/confirm, then confirm a repeat request finds nothing to do. scripts/run_demo.sh walks the same flow by hand.
source "$(dirname "$0")/common.sh"
need_build
test_init format2_update

gen_random_bin "$BASE" $((PAYLOAD_MAX * 5 / 10)) 901
cp "$BASE" "$IMAGE"
patch_sector "$IMAGE" $((SECTORS * 3 / 10)) 0x55
scramble_sector "$IMAGE" $((SECTORS * 6 / 10)) 9191

trap server_stop EXIT
server_start

# Provision + update + apply + verify + trial-boot + confirm, all through the default cb-client flow (no --session-only/--boot-only/--no-confirm).
out="$(update 2>&1)"
echo "$out" | grep -E 'plan v|device: wire|booting|trial|confirming'
wire_stats "$out"
plan_counts

assert_contains "$out" "HELLO + manifest sent" "the device identified itself and sent its manifest"
assert_contains "$out" "plan v2" "the server planned version 2"
grep -q "device staged the plan" "$RUN/server.log" || fail "server log" "the server did not see the device stage and ack the plan"
assert_contains "$out" "trial boot" "the staged image booted on trial, not an immediate commit"
assert_contains "$out" "attempt 1/3" "one trial attempt spent"
assert_contains "$out" "confirming slot B" "the default flow auto-confirmed the trial"
assert_contains "$out" "booting version 2" "version 2 is what actually booted"
[ "$WRITE_N" -ge 2 ] || fail "write count" "expected at least 2 writes (the two patched sectors), got $WRITE_N"

status="$("$CB_CLIENT" --flash "$FLASH" --status 2>&1)"
assert_contains "$status" "state COMMITTED" "the update is fully committed"
assert_contains "$status" "active slot B" "active_slot promoted to the updated slot"
assert_contains "$status" "slot B          image     version 2  verified" "the committed image verifies"

# Same release again: the device's active slot already matches it byte-for-byte (running_sha256), so the server reports nothing to offer before building a plan.
out2="$(update 2>&1)"
assert_contains "$out2" "no update offered" "a second request for the same release finds nothing to do"
grep -q "device already runs this image" "$RUN/server.log" || \
    fail "server log" "the server did not short-circuit on the matching running_sha256"

ok "format2_update (rx=$WIRE_RX B)"
