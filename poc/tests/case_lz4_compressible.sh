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

assert_contains "$out" "plan v2, 2 steps" "sector 0 (the version stamp) plus the motif sector"
assert_contains "$out" "booting version 2" "boots the new version"
assert_contains "$out" "verified" "image verifies"
[ "$(compress_count lz4)" -ge 1 ] || fail "compressor" "the motif sector was not sent as lz4"

rx="$(sed -n 's/.*rx=\([0-9]*\) bytes/\1/p' <<<"$out")"
limit=$((RAW_WIRE + 412))   # below the raw cost of both sectors
[ "$rx" -lt "$limit" ] || fail "wire size" "rx=$rx B, expected under $limit B"

ok "lz4_compressible (rx=$rx B)"
