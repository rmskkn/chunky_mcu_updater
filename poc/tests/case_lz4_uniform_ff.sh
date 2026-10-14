#!/usr/bin/env bash
# A uniform 0xFF sector that exists nowhere in the device's flash (so no COPY)
# must go over the wire LZ4-compressed, a few bytes instead of a raw sector.
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

assert_contains "$out" "plan v2, 2 steps" "sector 0 (the version stamp) plus the patched sector"
assert_contains "$out" "booting version 2" "boots the new version"
assert_contains "$out" "verified" "image verifies"
[ "$(compress_count lz4)" -ge 1 ] || fail "compressor" "the uniform sector was not sent as lz4"

rx="$(sed -n 's/.*rx=\([0-9]*\) bytes/\1/p' <<<"$out")"
# sector 0 is nearly incompressible, plus a tiny sector; ~412 B of handshake/acks
limit=$((RAW_WIRE + 412))
[ "$rx" -lt "$limit" ] || fail "wire size" "rx=$rx B, expected under $limit B (one raw-sized write + one tiny sector)"

ok "lz4_uniform_ff (rx=$rx B)"
