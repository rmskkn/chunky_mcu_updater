#!/usr/bin/env bash
# OP_COPY: the target wants sector 10 to hold exactly what sector 40 already
# holds in the device's current flash. The planner should resolve that as a
# copy (no bytes on the wire for sector 10) rather than a write, and the
# unbounded step pipeline (middleware/server_session.c) plus the device's step-at-a-time
# write path (lib/device.c) must not disturb a plan that mixes copies and
# writes with the ACK for each arriving out of send order.
source "$(dirname "$0")/common.sh"
need_build
test_init copy_dedup

# sector numbers scale with the geometry (10, 20, 40 at 1 KB)
S_DST=$((SECTORS / 10)); S_MID=$((SECTORS / 5)); S_SRC=$((SECTORS * 4 / 10))
gen_random_bin "$BASE" $((PAYLOAD_MAX * 6 / 10)) 103
cp "$BASE" "$IMAGE"
copy_sector "$IMAGE" $S_DST $S_SRC   # sector 10 now wants sector 40's bytes
scramble_sector "$IMAGE" $S_MID 999   # plus an ordinary write, so copy and write interleave

trap server_stop EXIT
server_start

out="$(update 2>&1)"
echo "$out" | grep -E 'plan v|device: wire|booting'
wire_stats "$out"

assert_contains "$out" "plan v2, 3 steps" "sector 0 (the version stamp), the write, and the copy"
assert_contains "$out" "booting version 2" "boots the new version"
assert_contains "$out" "verified" "image verifies"

# two writes (sector 0, the scrambled one) + a copy; ~524 B of handshake/acks
limit=$((2 * RAW_WIRE + 524))
[ "$WIRE_RX" -lt "$limit" ] || fail "wire size" "rx=$WIRE_RX B, expected under $limit B (two writes + one copy, not three writes)"

[ "$(compress_count raw)" -ge 1 ] || fail "raw fallback" "the scrambled sector did not stay raw"
grep -q "copied  $S_DST" "$RUN/server.log" || fail "copy step" "server log does not show sector 10 as copied"

ok "copy_dedup (rx=$WIRE_RX B)"
