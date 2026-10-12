#!/usr/bin/env bash
# Many ordinary STEP_WRITE sectors in one plan - exercises the unbounded
# step pipeline and the client's double-buffered flash worker (lib/client.c:
# flash_worker/stage_sector) under load: steps keep streaming in while
# earlier sectors are still (modelled as) programming, and every ACK/NAK
# must still land before the plan is persisted. Correctness is checked by
# re-reading every sector's hash after the update, not just by booting.
source "$(dirname "$0")/common.sh"
need_build
test_init many_writes

gen_random_bin "$BASE" 60000 104
cp "$BASE" "$IMAGE"
"$CB_FLASH" mutate "$BASE" "$IMAGE" 25 777 >/dev/null

trap server_stop EXIT
server_start

out="$(update 2>&1)"
echo "$out" | grep -E 'plan v|device: wire|booting'

steps="$(sed -n 's/.*plan v[0-9]*, \([0-9]*\) steps.*/\1/p' <<<"$out")"
[ "$steps" -ge 10 ] || fail "step count" "only $steps steps, expected a double-digit spread from 25 patch runs"
assert_contains "$out" "booting version 2" "boots the new version"
assert_contains "$out" "verified" "image verifies"
# The apply loop re-checks every sector's hash against the plan, and verify
# re-checks image_sha256 plus the signature over the whole slot - so these
# two failure strings covering both checks are the sign anything is wrong.
assert_not_contains "$out" "does not match the plan" "every sector matched its planned hash"
assert_not_contains "$out" "does not verify" "the staged image verified"

ok "many_writes ($steps steps)"
