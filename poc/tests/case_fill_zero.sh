#!/usr/bin/env bash
# STEP_FILL, 0x00 case: same as case_fill_erased.sh but the repeated byte is
# 0x00 rather than the erased 0xFF, exercising the other branch of the
# fillable check in lib/update_server.c.
source "$(dirname "$0")/common.sh"
need_build
test_init fill_zero

gen_random_bin "$BASE" 104320 102
cp "$BASE" "$IMAGE"
patch_sector "$IMAGE" 30 0x00

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

ok "fill_zero (rx=$rx B)"
