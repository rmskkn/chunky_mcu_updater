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
# S_SRC is never a candidate, lib/plan.c checks the active slot first and
# only falls back to the destination's own unchanged sectors -- so S_DST is
# copied from there for free. If the planner ever regressed to picking the
# unsafe destination-local source instead, S_DST would end up holding
# S_SRC's *new* bytes and the staged image would fail its verify at boot,
# which is what the "booting version 2" / "verified" assertions below
# would catch.
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

# Only sector 0 and S_SRC need to be sent; S_DST is a free copy from the
# active slot. (Everything else the destination doesn't already hold is
# also a free copy, since the destination starts blank -- see
# case_copy_dedup.sh's comment on the same effect.)
grep -q "2 sent, " "$RUN/server.log" || fail "write count" \
    "expected exactly 2 writes (sector 0 and S_SRC); S_DST should be a copy; see $RUN/server.log"
assert_contains "$out" "booting version 2" "boots the new version: proves S_DST got S_SRC's OLD bytes, from the active slot"
assert_contains "$out" "verified" "image verifies: a copy from the unsafe (rewritten) destination source would fail this"

ok "copy_src_rewritten (rx=$WIRE_RX B)"
