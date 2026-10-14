#!/usr/bin/env bash
# LZ4 with the old sector as dictionary. A release that shifts code by 16 bytes
# changes every sector after the insertion, yet each is a displaced copy of what
# the device already holds, so the dictionary should collapse them. Needs the
# server's sector store to have seen the previous release (step 1).
source "$(dirname "$0")/common.sh"
need_build
test_init lz4_dict

gen_random_bin "$BASE" $PAYLOAD_MAX 107
R1="$RUN/release1.bin"; R2="$RUN/release2.bin"; R3="$RUN/release3.bin"; R4="$RUN/release4.bin"
S_INS=$((SECTORS * 4 / 10))      # 40 at 1 KB
S_TWEAK=$((SECTORS * 7 / 10))    # 71 at 1 KB

trap server_stop EXIT
server_start

# 1. one scrambled sector: populates the store with release 1
cp "$BASE" "$R1"; scramble_sector "$R1" $((SECTORS / 10)) 4242
cp "$R1" "$IMAGE"
out="$(update 2>&1)"
assert_contains "$out" "booting version 2" "step 1 boots v2"

# 2. insertion: the tail shifts by 16 B
cp "$R1" "$R2"; insert_bytes "$R2" $S_INS 16
cp "$R2" "$IMAGE"
out="$(update 2>&1)"
echo "$out" | grep -E 'plan v|device: wire|booting'
assert_contains "$out" "booting version 3" "step 2 boots v3"
assert_contains "$out" "verified" "step 2 image verifies"
steps="$(sed -n 's/.*plan v[0-9]*, \([0-9]*\) steps.*/\1/p' <<<"$out")"
dict="$(compress_count lz4_dict)"
[ $((dict * 10)) -ge $((steps * 9)) ] || fail "dictionary use" "lz4_dict=$dict of $steps written sectors, expected at least 90%"
rx="$(sed -n 's/.*rx=\([0-9]*\) bytes/\1/p' <<<"$out")"
[ $((rx * 100)) -lt $((steps * SECTOR_SIZE * 15)) ] || fail "wire size" "rx=$rx B for $steps sectors, expected under 15% of $((steps * SECTOR_SIZE)) B"
RAW_COST=$((steps * RAW_WIRE))

# 3. a small change to one sector, store intact: dictionary-coded
cp "$R2" "$R3"; tweak_sector "$R3" $S_TWEAK
cp "$R3" "$IMAGE"
out="$(update 2>&1)"
assert_contains "$out" "booting version 4" "step 3 boots v4"
[ "$(compress_count raw)" -eq 0 ] || fail "control" "sector sent raw although the store is intact"

# 4. the same, with the store file for the device's current sector corrupted
#    (same name, same size, different bytes): it must be a miss, not a bad dictionary
h="$(sector_hash_hex "$FLASH" $S_TWEAK)"
gen_random_bin "$CACHE/sectors/$h" $SECTOR_SIZE 9
cp "$R3" "$R4"; tweak_sector "$R4" $S_TWEAK 0x33
cp "$R4" "$IMAGE"
out="$(update 2>&1)"
assert_contains "$out" "booting version 5" "step 4 boots v5"
assert_contains "$out" "verified" "step 4 image verifies"
[ "$(compress_count raw)" -eq 1 ] || fail "store tamper" "the corrupted store entry was still used as a dictionary"

ok "lz4_dict (step 2: $steps sectors, lz4_dict=$dict, rx=$rx B vs $RAW_COST B raw)"
