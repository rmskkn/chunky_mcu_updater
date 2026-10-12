#!/usr/bin/env bash
# STEP_FILL, 0xFF case: a sector whose target content is uniformly erased,
# and which does not already exist anywhere in the device's current flash
# (so the planner cannot resolve it as a cheap COPY) - the only way to reach
# it is a WRITE, and the server should send it as one small STEP_FILL frame
# instead of four STEP_WRITE fragments. lib/update_server.c, the "fillable" check.
source "$(dirname "$0")/common.sh"
need_build
test_init fill_erased

gen_random_bin "$BASE" 104320 101
cp "$BASE" "$IMAGE"
patch_sector "$IMAGE" 30 0xFF

trap server_stop EXIT
server_start

out="$(update 2>&1)"
echo "$out" | grep -E 'plan v|device: wire|booting'

assert_contains "$out" "plan v2, 2 steps" "sector 0 (the version stamp) plus the one patched sector"
assert_contains "$out" "booting version 2" "boots the new version"
assert_contains "$out" "verified" "image verifies"

rx="$(sed -n 's/.*rx=\([0-9]*\) bytes/\1/p' <<<"$out")"
# One real STEP_WRITE (sector 0, the version stamp) plus one compressed
# STEP_FILL. Two full STEP_WRITEs would run past 2.1 KB, so the ceiling
# below still tells a fill apart from a write for the patched sector.
[ "$rx" -lt 1500 ] || fail "wire size" "rx=$rx B, expected under ~1.5 KB (one write + one fill, not two writes)"

ok "fill_erased (rx=$rx B)"
