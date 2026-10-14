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

assert_contains "$out" "plan v2, 2 steps" "sector 0 (the version stamp) plus the patched sector"
assert_contains "$out" "booting version 2" "boots the new version"
assert_contains "$out" "verified" "image verifies"
[ "$(compress_count lz4)" -ge 1 ] || fail "compressor" "the uniform sector was not sent as lz4"

rx="$(sed -n 's/.*rx=\([0-9]*\) bytes/\1/p' <<<"$out")"
limit=$((RAW_WIRE + 412))
[ "$rx" -lt "$limit" ] || fail "wire size" "rx=$rx B, expected under $limit B (one raw-sized write + one tiny sector)"

ok "lz4_uniform_00 (rx=$rx B)"
