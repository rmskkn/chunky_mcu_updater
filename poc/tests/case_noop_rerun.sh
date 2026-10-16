#!/usr/bin/env bash
# Baseline regression: asking twice for a release the device already runs
# must short-circuit at NO_UPDATE before any plan is built - unaffected by
# the pipelining and double-buffer changes, since no steps are ever sent.
source "$(dirname "$0")/common.sh"
need_build
test_init noop_rerun

gen_random_bin "$BASE" $((PAYLOAD_MAX * 4 / 10)) 105
cp "$BASE" "$IMAGE"
patch_sector "$IMAGE" $((SECTORS * 15 / 100)) 0xFF   # a real difference, so the first run actually updates

trap server_stop EXIT
server_start

first="$(update 2>&1)"
wire_stats "$first"
assert_contains "$first" "plan v2" "first run finds a real difference and stages it"
assert_contains "$first" "booting version 2" "first run installs version 2"

second="$(update 2>&1)"
wire_stats "$second"
assert_contains "$second" "no update offered" "second run finds nothing to do"
[ "$WIRE_RX" -lt 200 ] || fail "wire size" "rx=$WIRE_RX B on a no-op request, expected a bare handshake"

ok "noop_rerun (rx=$WIRE_RX B)"
