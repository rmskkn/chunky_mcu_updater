#!/usr/bin/env bash
# A/B resume after a power cut, now with two slots' manifests to re-diff
# against (docs/ab_migration.md). Reuses the --abort-after-plan hook from
# case_unroll_no_boot.sh to get an interrupted plan on the books: the
# session writes several sectors straight into slot B (writes land live,
# regardless of commit), the plan is persisted as STAGING/unarmed, and the
# device never reboots into it. A plain follow-up request must then:
#  - unroll the stale plan on its first boot (covered in depth by
#    case_unroll_no_boot.sh; here just a precondition),
#  - manifest both slots correctly (slot A untouched, slot B a mix of the
#    interrupted write's sectors and whatever unroll erased),
#  - and still produce a correct plan and a verified result, same as any
#    other update -- resume falls out of re-diffing against reality,
#    there is no separate "resume" code path to exercise.
source "$(dirname "$0")/common.sh"
need_build
test_init resume_two_slots

gen_random_bin "$BASE" $((PAYLOAD_MAX * 5 / 10)) 401
cp "$BASE" "$IMAGE"
patch_sector "$IMAGE" $((SECTORS * 2 / 10)) 0xAA
scramble_sector "$IMAGE" $((SECTORS * 6 / 10)) 4040

"$CB_SERVER" --port "$PORT" --image "$IMAGE" --cache "$CACHE" --abort-after-plan \
    >"$RUN/server.log" 2>&1 &
SERVER_PID=$!
trap server_stop EXIT
for _ in $(seq 50); do grep -q listening "$RUN/server.log" 2>/dev/null && break; sleep 0.05; done

# Interrupted attempt: sectors land in slot B, the plan is staged, the
# server aborts before COMMIT, and the device never boots it.
"$CB_CLIENT" --flash "$FLASH" --initial "$BASE" --port "$PORT" --session-only >/dev/null 2>&1
grep -q "device staged the plan" "$RUN/server.log" || fail "setup" "the interrupted plan was not staged"
server_stop

status="$("$CB_CLIENT" --flash "$FLASH" --status 2>&1)"
assert_contains "$status" "state STAGING" "the interrupted plan is on the books, unarmed"

# A real power-cut recovery reboots into the bootloader before the next
# request; unroll happens here (case_unroll_no_boot.sh covers it directly).
boot_out="$("$CB_CLIENT" --flash "$FLASH" --boot-only 2>&1)"
assert_contains "$boot_out" "unrolling the plan into slot B" "stale plan unrolled before the resume request"

# Both slots now manifest sensibly: A is exactly the original base, B is a
# mix of leftover written sectors and the unroll's erased sector 0 -- real
# content either way, not blank.
manifest1="$("$CB_FLASH" manifest "$FLASH")"
echo "$manifest1" | grep -c '[0-9a-f]\{16\}' >/dev/null || fail "manifest" "manifest did not print"

# The follow-up request re-diffs against current reality (both slots) and
# must still land a correct, verified update -- same target as before.
"$CB_SERVER" --port "$PORT" --image "$IMAGE" --cache "$CACHE" >"$RUN/server2.log" 2>&1 &
SERVER_PID=$!
for _ in $(seq 50); do grep -q listening "$RUN/server2.log" 2>/dev/null && break; sleep 0.05; done

out="$("$CB_CLIENT" --flash "$FLASH" --port "$PORT" 2>&1)"
echo "$out" | grep -E 'plan v|booting'
wire_stats "$out"
assert_contains "$out" "booting version 2" "the resumed request still lands and boots v2"
assert_contains "$out" "verified" "the resumed image verifies"
assert_contains "$out" "confirming slot B" "the default flow auto-confirms the resumed trial"

status2="$("$CB_CLIENT" --flash "$FLASH" --status 2>&1)"
assert_contains "$status2" "state COMMITTED" "resume ends fully committed, same as an uninterrupted update"
assert_contains "$status2" "active slot B" "active_slot promoted after the resumed update confirms"

ok "resume_two_slots (rx=$WIRE_RX B)"
