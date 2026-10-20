#!/usr/bin/env bash
# A/B unroll before any boot attempt (DESIGN.md section 8): a plan that is
# staged (persisted at PLAN_END) but never armed (no COMMIT) must be
# unrolled by the next boot, without ever having booted the pending
# image. The PoC has no server-side way to withhold COMMIT in the real
# protocol flow, so cb-server's test-only --abort-after-plan sends ABORT
# right after PLAN_READY instead, landing the device in exactly the state
# a link drop between PLAN_READY and COMMIT would (lib/device.c's
# on_commit(): anything other than MSG_COMMIT leaves the plan unarmed).
source "$(dirname "$0")/common.sh"
need_build
test_init unroll_no_boot

gen_random_bin "$BASE" $((PAYLOAD_MAX * 5 / 10)) 301
cp "$BASE" "$IMAGE"
patch_sector "$IMAGE" $((SECTORS * 2 / 10)) 0x99

"$CB_SERVER" --port "$PORT" --image "$IMAGE" --cache "$CACHE" --abort-after-plan \
    >"$RUN/server.log" 2>&1 &
SERVER_PID=$!
trap server_stop EXIT
for _ in $(seq 50); do grep -q listening "$RUN/server.log" 2>/dev/null && break; sleep 0.05; done

# Session only: the server aborts before COMMIT, so the client never calls
# bootloader_boot() itself (--boot-only below is the first boot call).
out="$("$CB_CLIENT" --flash "$FLASH" --initial "$BASE" --port "$PORT" --session-only 2>&1)"
grep -q "device staged the plan" "$RUN/server.log" || fail "server log" "the plan did not reach PLAN_END and get persisted"
grep -q "sending ABORT instead of COMMIT" "$RUN/server.log" || fail "server hook" "the test server did not abort before commit"

status="$("$CB_CLIENT" --flash "$FLASH" --status 2>&1)"
assert_contains "$status" "state STAGING" "the plan is persisted as STAGING, unarmed"

# The first-ever boot call must unroll, not apply: it never touches the
# pending image at all.
boot_out="$("$CB_CLIENT" --flash "$FLASH" --boot-only 2>&1)"
assert_contains "$boot_out" "unrolling the plan into slot B" "unroll fires on the very first boot, before any apply/trial"
assert_contains "$boot_out" "plan was never committed" "unroll's reason is the unarmed plan, not a verify failure"
assert_not_contains "$boot_out" "trial boot" "the pending image was never booted, not even once"
assert_contains "$boot_out" "booting version 1" "falls back to the unchanged active slot"

status2="$("$CB_CLIENT" --flash "$FLASH" --status 2>&1)"
assert_contains "$status2" "state NONE" "no plan in flight after unroll"
assert_contains "$status2" "active slot A" "active_slot never moved"
assert_contains "$status2" "slot B          no image" "slot B's header (sector 0) was erased by unroll"

ok "unroll_no_boot"
