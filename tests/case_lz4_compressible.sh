#!/usr/bin/env bash
# A repetitive but not uniform sector (a 16-byte motif) must also compress.
source "$(dirname "$0")/common.sh"
need_build
test_init lz4_compressible

gen_random_bin "$BASE" $PAYLOAD_MAX 106
cp "$BASE" "$IMAGE"
motif_sector "$IMAGE" $((SECTORS * 3 / 10))

trap server_stop EXIT
server_start

out="$(update 2>&1)"
echo "$out" | grep -E 'plan v|device: wire|booting'
wire_stats "$out"

# Exactly 2 sectors sent (sector 0, the motif sector); everything else is a
# free OP_COPY from the active slot (the destination starts blank on the
# device's first A/B update), which inflates the step count without
# costing wire bytes -- "2 sent" is the stable invariant, not step count.
grep -q "2 sent, " "$RUN/server.log" || fail "write count" \
    "expected exactly 2 writes (sector 0 and the motif sector); see $RUN/server.log"
assert_contains "$out" "booting version 2" "boots the new version"
assert_contains "$out" "verified" "image verifies"
[ "$(compress_count lz4)" -ge 1 ] || fail "compressor" "the motif sector was not sent as lz4"

# two writes, both compressible, plus many free copies of everything else.
limit=$((RAW_WIRE + COPY_MARGIN))
[ "$WIRE_RX" -lt "$limit" ] || fail "wire size" "rx=$WIRE_RX B, expected under $limit B"

ok "lz4_compressible (rx=$WIRE_RX B)"
