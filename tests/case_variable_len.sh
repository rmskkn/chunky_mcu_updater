#!/usr/bin/env bash
# Variable image_len (DESIGN.md section 10.1): the image is its real size
# rounded up to a sector, not the whole slot. A bigger release grows the plan,
# a smaller one shrinks it, and each one still verifies and trial-boots; the
# stale tail a shrink leaves behind is outside the hash domain and harmless.
source "$(dirname "$0")/common.sh"
need_build
test_init variable_len

sectors_of() { echo $((($1 + DESC_LEN + SECTOR_SIZE - 1) / SECTOR_SIZE)); }

small=$((PAYLOAD_MAX * 3 / 10))
big=$((PAYLOAD_MAX * 7 / 10))
tiny=$((PAYLOAD_MAX * 1 / 10))

gen_random_bin "$BASE" "$small" 601
cp "$BASE" "$IMAGE"
server_start
trap server_stop EXIT

# plan_line OUT: "<steps> <sectors>" from the device's PLAN_BEGIN log line.
plan_line() { sed -n 's/.*plan v[0-9]*, \([0-9]*\) steps over \([0-9]*\) sectors.*/\1 \2/p' <<<"$1" | tail -1; }

run() {   # label size seed expected_version expected_active
    gen_random_bin "$IMAGE" "$2" "$3"
    out="$(update 2>&1)"
    wire_stats "$out"
    read -r steps n <<<"$(plan_line "$out")"
    [ "$n" = "$(sectors_of "$2")" ] || fail "$1" "plan covers $n sectors, expected $(sectors_of "$2")"
    [ "$steps" -le "$n" ] || fail "$1" "$steps steps for a $n-sector image"
    assert_contains "$out" "booting version $4" "$1: trial boot of the new image"
    status="$("$CB_CLIENT" --flash "$FLASH" --status 2>&1)"
    assert_contains "$status" "active slot $5" "$1: confirmed"
    assert_not_contains "$status" "mismatch" "$1: both slots still verify"
    assert_contains "$status" "version $4  verified" "$1: the new slot verifies"
    LAST_STEPS=$steps; LAST_N=$n
}

run "grow"    "$big"   602 2 B
[ "$LAST_N" -gt "$(sectors_of "$small")" ] || fail "grow" "the plan did not grow"
grow_steps=$LAST_STEPS

run "shrink"  "$tiny"  603 3 A
[ "$LAST_N" -lt "$(sectors_of "$big")" ] || fail "shrink" "the plan did not shrink"
[ "$LAST_STEPS" -lt "$grow_steps" ] || fail "shrink" "fewer sectors should mean fewer steps ($LAST_STEPS vs $grow_steps)"

run "regrow"  "$big"   604 4 B

ok "variable_len"
