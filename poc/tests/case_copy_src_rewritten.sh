#!/usr/bin/env bash
# OP_COPY whose source the same plan rewrites. The target wants sector 10 to
# hold what sector 40 holds today, and sector 40 itself changes. In place,
# WRITEs land during the session and COPYs only at boot, so by the time the
# bootloader copies, sector 40 already holds its new bytes. The planner must
# not pick a source the plan writes; sector 10 has to go over the wire.
source "$(dirname "$0")/common.sh"
need_build
test_init copy_src_rewritten

S_DST=$((SECTORS / 10)); S_SRC=$((SECTORS * 4 / 10))
gen_random_bin "$BASE" $((PAYLOAD_MAX * 6 / 10)) 106
cp "$BASE" "$IMAGE"
copy_sector "$IMAGE" $S_DST $S_SRC     # sector 10 now wants sector 40's old bytes
scramble_sector "$IMAGE" $S_SRC 4242   # ...and sector 40 gets new ones

trap server_stop EXIT
server_start

out="$(update 2>&1)"
echo "$out" | grep -E 'plan v|device: wire|booting|copy source'
wire_stats "$out"

assert_not_contains "$out" "copy source" "no copy from a sector the plan rewrites"
assert_contains "$out" "booting version 2" "boots the new version"
assert_contains "$out" "verified" "image verifies"
grep -q "copied  $S_DST" "$RUN/server.log" && fail "copy step" "sector $S_DST planned as a copy of a rewritten sector"

ok "copy_src_rewritten (rx=$WIRE_RX B)"
