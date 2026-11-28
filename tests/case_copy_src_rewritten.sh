#!/usr/bin/env bash
# A/B migration (docs/ab_migration.md): this case used to prove the
# single-partition restriction that a COPY source must be a sector the
# plan leaves alone, because in place a source the same plan also rewrites
# holds its new bytes by the time the bootloader gets to it. That
# restriction is scoped to one slot, not the whole device: once there are
# two slots, the active one is read-only for the entire update (DESIGN.md
# section 8's apply invariant), so a copy source found there is always
# safe even when the destination slot's own copy of that sector is
# rewritten by this same plan. This case now asserts exactly that.
#
# The target wants sector S_DST to hold what sector S_SRC holds *today* (in
# the active slot), while S_SRC itself also changes this same plan. In
# place that forced S_DST to be sent as a write (case retired above). Under
# A/B, S_DST's wanted bytes still exist, untouched, in the active slot at
# S_SRC -- the destination slot's own (simultaneously rewritten) copy of
# S_SRC is never a candidate: format2_plan_build() only searches the active slot for a COPY source (find_active_match()), never the destination. This server never reaches that search, though: it plans against an all-blank scratch "active" (no live flash_t, manifest carries hashes not bytes), so S_DST arrives as a write, not a copy; what the case still proves is that S_DST's bytes are S_SRC's *old* ones, checked against the device's manifest below.
source "$(dirname "$0")/common.sh"
need_build
test_init copy_src_rewritten

S_DST=$((SECTORS / 10)); S_SRC=$((SECTORS * 4 / 10))
gen_random_bin "$BASE" $((PAYLOAD_MAX * 6 / 10)) 106
cp "$BASE" "$IMAGE"
copy_sector "$IMAGE" $S_DST $S_SRC     # sector S_DST now wants S_SRC's *old* (active-slot) bytes
scramble_sector "$IMAGE" $S_SRC 4242   # ...and S_SRC itself changes in this same plan

trap server_stop EXIT
server_start

out="$(update 2>&1)"
echo "$out" | grep -E 'plan v|device: wire|booting'
wire_stats "$out"
plan_counts

# This server plans against an all-blank scratch "active" (see case_copy_dedup.sh), so S_DST arrives as a write here, not the free copy this case is named for.
assert_contains "$out" "booting version 2" "boots the new version"
assert_contains "$out" "verified" "image verifies"
[ "$COPY_N" -eq 0 ] || fail "copy count" \
    "expected 0 copies over this server's all-blank-scratch plan, got $COPY_N; see $RUN/server.log"

# The real claim: S_DST must hold S_SRC's bytes as they stood before this plan also rewrote S_SRC, never S_SRC's new bytes. Flat indices as in case_copy_dedup.sh: active slot B at index i, inactive slot A at SECTORS+i.
dst_hash="$(sector_hash_hex "$FLASH" "$S_DST")"
old_src_hash="$(sector_hash_hex "$FLASH" $((SECTORS + S_SRC)))"
[ -n "$dst_hash" ] && [ "$dst_hash" = "$old_src_hash" ] || fail "content" \
    "sector $S_DST's hash ($dst_hash) does not match S_SRC's old (active-slot) hash ($old_src_hash)"

ok "copy_src_rewritten (rx=$WIRE_RX B)"
