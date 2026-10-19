#!/usr/bin/env bash
# Same as case_lz4_uniform_ff.sh with 0x00, the other common padding byte.
source "$(dirname "$0")/common.sh"
need_build
test_init lz4_uniform_00

gen_random_bin "$BASE" $PAYLOAD_MAX 102
cp "$BASE" "$IMAGE"
patch_sector "$IMAGE" $((SECTORS * 3 / 10)) 0x00

trap server_stop EXIT
server_start

out="$(update 2>&1)"
echo "$out" | grep -E 'plan v|device: wire|booting'
wire_stats "$out"

# Exactly 2 sectors sent (sector 0, the patched sector); everything else is
# a free OP_COPY from the active slot (the destination starts blank on the
# device's first A/B update), which inflates the step count without
# costing wire bytes -- "2 sent" is the stable invariant, not step count.
# (0x00 is not the blank/erased pattern, unlike 0xFF -- see
# case_lz4_uniform_ff.sh -- so this sector still needs to be sent.)
grep -q "2 sent, " "$RUN/server.log" || fail "write count" \
    "expected exactly 2 writes (sector 0 and the patched sector); see $RUN/server.log"
assert_contains "$out" "booting version 2" "boots the new version"
assert_contains "$out" "verified" "image verifies"
[ "$(compress_count lz4)" -ge 1 ] || fail "compressor" "the uniform sector was not sent as lz4"

limit=$((RAW_WIRE + COPY_MARGIN))
[ "$WIRE_RX" -lt "$limit" ] || fail "wire size" "rx=$WIRE_RX B, expected under $limit B (two small writes, many free copies)"

ok "lz4_uniform_00 (rx=$WIRE_RX B)"
