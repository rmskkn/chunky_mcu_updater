#!/usr/bin/env bash
# format2's image_len is fixed per geometry (FORMAT2_IMAGE_LEN = SLOT_SIZE - 128), not per-release: a smaller binary is just 0xFF padding, and a blank padding chunk is skipped outright (no step, SPEC.md 7.3), so shrinking costs near-zero extra wire bytes over the sectors that actually changed. Each release still verifies and trial-boots regardless of size.
source "$(dirname "$0")/common.sh"
need_build
test_init variable_len

small=$((PAYLOAD_MAX * 3 / 10))
big=$((PAYLOAD_MAX * 7 / 10))
tiny=$((PAYLOAD_MAX * 1 / 10))

gen_random_bin "$BASE" "$small" 601
cp "$BASE" "$IMAGE"
server_start
trap server_stop EXIT

run() {   # label size seed expected_version expected_active
    gen_random_bin "$IMAGE" "$2" "$3"
    out="$(update 2>&1)"
    wire_stats "$out"
    plan_counts
    assert_contains "$out" "booting version $4" "$1: trial boot of the new image"
    status="$("$CB_CLIENT" --flash "$FLASH" --status 2>&1)"
    assert_contains "$status" "active slot $5" "$1: confirmed"
    assert_not_contains "$status" "mismatch" "$1: both slots still verify"
    assert_contains "$status" "version $4  verified" "$1: the new slot verifies"
    LAST_WRITE=$WRITE_N; LAST_RX=$WIRE_RX
}

run "grow"    "$big"   602 2 B
grow_rx=$LAST_RX

run "shrink"  "$tiny"  603 3 A
# Shrinking should cost far less than growing: more of the slot is now blank padding, skipped outright, so fewer writes and nowhere near a full image's worth of bytes.
[ "$LAST_RX" -lt "$grow_rx" ] || fail "shrink" "rx=$LAST_RX B did not drop below the grow step's $grow_rx B"
[ "$LAST_RX" -lt $((tiny + SECTOR_SIZE * 4)) ] || fail "shrink" \
    "rx=$LAST_RX B, expected close to the $tiny B binary itself (near-zero extra cost for the sectors that just went blank)"

run "regrow"  "$big"   604 4 B

ok "variable_len"
