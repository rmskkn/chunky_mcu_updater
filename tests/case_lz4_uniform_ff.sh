#!/usr/bin/env bash
# A uniform 0xFF sector is the erased pattern -- under A/B it now always
# "exists" at the destination for free, because a blank (never-written)
# inactive slot reads back as all-0xFF. So the planner's rule 1 ("already
# correct") fires directly: this sector costs zero wire bytes, not a
# compressed few, which is a strictly better outcome than the single-
# partition case this test originally exercised (where no second slot
# meant nothing already matched). Compare with case_lz4_uniform_00.sh,
# where 0x00 is not the erased pattern and still has to be sent.
source "$(dirname "$0")/common.sh"
need_build
test_init lz4_uniform_ff

gen_random_bin "$BASE" $PAYLOAD_MAX 101
cp "$BASE" "$IMAGE"
patch_sector "$IMAGE" $((SECTORS * 3 / 10)) 0xFF

trap server_stop EXIT
server_start

out="$(update 2>&1)"
echo "$out" | grep -E 'plan v|device: wire|booting'
wire_stats "$out"

# Exactly 1 sector sent: sector 0 (the version stamp). The patched 0xFF
# sector matches the blank destination directly (rule 1) and is never even
# a step, let alone a write -- the strictly-better-than-compression outcome
# described above.
grep -q "1 sent, " "$RUN/server.log" || fail "write count" \
    "expected exactly 1 write (sector 0 only); the 0xFF sector should match the blank destination for free; see $RUN/server.log"
grep -qE "sectors:.*differ; 1 sent" "$RUN/server.log" || fail "skip" \
    "expected the patched sector among the 'already right' count, not sent or copied"
assert_contains "$out" "booting version 2" "boots the new version"
assert_contains "$out" "verified" "image verifies"

# one small write (sector 0, likely compressible) plus many free copies of
# everything else; the patched sector itself costs nothing at all.
limit=$((RAW_WIRE + COPY_MARGIN))
[ "$WIRE_RX" -lt "$limit" ] || fail "wire size" "rx=$WIRE_RX B, expected under $limit B"

ok "lz4_uniform_ff (rx=$WIRE_RX B)"
