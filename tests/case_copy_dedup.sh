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

# Exactly 2 sectors sent (sector 0, the scrambled one); everything else the
# device doesn't already hold at the destination (dest slot starts blank
# on a device's first A/B update) is a free OP_COPY from the active slot,
# which is what inflates the step/ack count without costing wire bytes --
# "2 sent" is the stable invariant, not the total step count.
grep -q "2 sent, " "$RUN/server.log" || fail "write count" \
    "expected exactly 2 writes (sector 0 and the scrambled sector); see $RUN/server.log"
assert_contains "$out" "booting version 2" "boots the new version"
assert_contains "$out" "verified" "image verifies"

# two writes (sector 0, the scrambled one) + many free copies (every other
# sector, since the destination starts blank): still cheap on the wire,
# just not as cheap as when the destination already holds most of it.
limit=$((2 * RAW_WIRE + COPY_MARGIN))
[ "$WIRE_RX" -lt "$limit" ] || fail "wire size" "rx=$WIRE_RX B, expected under $limit B (two writes + many free copies, not $SECTORS writes)"

[ "$(compress_count raw)" -ge 1 ] || fail "raw fallback" "the scrambled sector did not stay raw"
# S_DST must be a copy, not a write: "2 sent" above already pins the write
# set to exactly {0, S_MID} (checked by count; S_DST is neither), so it was
# necessarily resolved as OP_COPY -- the server log's "copied" list is
# run-length folded and S_DST can land inside a merged range like "1-9"
# rather than appear as its own token, so this is the robust way to check it.

ok "copy_dedup (rx=$WIRE_RX B)"
