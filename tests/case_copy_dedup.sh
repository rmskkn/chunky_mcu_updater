#!/usr/bin/env bash
# COPY was the original point (sector 10 == sector 40's bytes, resolvable by format2_plan_build()'s find_active_match()), but this server plans against an all-blank scratch "active" (no live flash_t, manifest carries hashes not bytes), so every non-blank chunk comes back a write, never a copy; this case instead proves the result is byte-correct wherever content was duplicated.
source "$(dirname "$0")/common.sh"
need_build
test_init copy_dedup

# sector numbers scale with the geometry (10, 20, 40 at 1 KB)
S_DST=$((SECTORS / 10)); S_MID=$((SECTORS / 5)); S_SRC=$((SECTORS * 4 / 10))
gen_random_bin "$BASE" $((PAYLOAD_MAX * 6 / 10)) 103
cp "$BASE" "$IMAGE"
copy_sector "$IMAGE" $S_DST $S_SRC   # sector 10 now wants sector 40's bytes
scramble_sector "$IMAGE" $S_MID 999   # plus an ordinary rewrite elsewhere

trap server_stop EXIT
server_start

out="$(update 2>&1)"
echo "$out" | grep -E 'plan v|device: wire|booting'
wire_stats "$out"
plan_counts

assert_contains "$out" "booting version 2" "boots the new version"
assert_contains "$out" "verified" "image verifies"
[ "$COPY_N" -eq 0 ] || fail "copy count" \
    "expected 0 copies over this server's all-blank-scratch plan, got $COPY_N; see $RUN/server.log"

# S_DST must hold S_SRC's bytes (written, not copied, per above); after update()'s auto-confirm, active_slot is B, so the manifest puts active slot B's chunks at flat index i and inactive slot A's at SECTORS+i.
dst_hash="$(sector_hash_hex "$FLASH" "$S_DST")"
src_hash="$(sector_hash_hex "$FLASH" $((SECTORS + S_SRC)))"
[ -n "$dst_hash" ] && [ "$dst_hash" = "$src_hash" ] || fail "content" \
    "sector $S_DST's hash ($dst_hash) does not match sector $S_SRC's ($src_hash)"

ok "copy_dedup (rx=$WIRE_RX B)"
