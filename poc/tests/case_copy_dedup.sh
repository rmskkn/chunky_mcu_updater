#!/usr/bin/env bash
# OP_COPY: the target wants sector 10 to hold exactly what sector 40 already
# holds in the device's current flash. The planner should resolve that as a
# copy (no bytes on the wire for sector 10) rather than a write, and the
# unbounded step pipeline (lib/update_server.c) plus the client's double-buffered
# write path (lib/client.c) must not disturb a plan that mixes copies and
# writes with the ACK for each arriving out of send order.
source "$(dirname "$0")/common.sh"
need_build
test_init copy_dedup

gen_random_bin "$BASE" 60000 103
cp "$BASE" "$IMAGE"
copy_sector "$IMAGE" 10 40        # sector 10 now wants sector 40's bytes
scramble_sector "$IMAGE" 20 999   # plus an ordinary write, so copy and write interleave

trap server_stop EXIT
server_start

out="$(update 2>&1)"
echo "$out" | grep -E 'plan v|device: wire|booting'

assert_contains "$out" "plan v2, 3 steps" "sector 0 (the version stamp), the write, and the copy"
assert_contains "$out" "booting version 2" "boots the new version"
assert_contains "$out" "verified" "image verifies"

rx="$(sed -n 's/.*rx=\([0-9]*\) bytes/\1/p' <<<"$out")"
# Two real STEP_WRITEs (sector 0 and sector 20) plus one STEP_COPY that
# carries no sector bytes at all. Three full writes would run past 3.2 KB.
[ "$rx" -lt 2700 ] || fail "wire size" "rx=$rx B, expected under ~2.7 KB (two writes + one copy, not three writes)"

grep -q 'copied  10' "$RUN/server.log" || fail "copy step" "server log does not show sector 10 as copied"

ok "copy_dedup (rx=$rx B)"
