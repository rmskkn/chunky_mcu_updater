#!/usr/bin/env bash
# Many WRITE/COPY steps in one plan: format2's bulk delivery applies the whole plan body in one pass during the session (no overlapped/streaming application). Checked by comparing every sector's hash from the device's own manifest against the release file, not just by booting.
source "$(dirname "$0")/common.sh"
need_build
test_init many_writes

gen_random_bin "$BASE" $((PAYLOAD_MAX * 6 / 10)) 104
cp "$BASE" "$IMAGE"
"$CB_FLASH" mutate "$BASE" "$IMAGE" 25 777 >/dev/null

trap server_stop EXIT
server_start

out="$(update 2>&1)"
echo "$out" | grep -E 'plan v|device: wire|booting'
wire_stats "$out"
plan_counts

# larger sectors make the 25 runs collide more
need=$((SECTORS / 10 < 10 ? SECTORS / 10 : 10))
[ "$WRITE_N" -ge "$need" ] || fail "write count" "only $WRITE_N writes, expected at least $need from 25 patch runs"
assert_contains "$out" "booting version 2" "boots the new version"
assert_contains "$out" "verified" "image verifies"

# Post-hoc check: every sector format2_apply_step() wrote/copied into slot B must read back as the release's own bytes, via the device's manifest. After update()'s auto-confirm, active_slot is B, so the manifest puts its chunks at flat index i, not SECTORS+i.
python3 -I -c "
import hashlib, subprocess, sys
SECT = $SECTOR_SIZE
data = open('$IMAGE', 'rb').read()
full = len(data) // SECT
out = subprocess.run(['$CB_FLASH', 'manifest', '$FLASH'], capture_output=True, text=True, check=True).stdout
tok = out.split()
got = {}
for j in range(0, len(tok) - 1, 2):
    got[tok[j]] = tok[j + 1]
bad = []
for i in range(full):
    want = hashlib.sha256(data[i * SECT:(i + 1) * SECT]).digest()[:8].hex()
    have = got.get(str(i))
    if have != want:
        bad.append((i, want, have))
if bad:
    print('sector hash mismatch:', bad[:5], file=sys.stderr)
    sys.exit(1)
print(full, 'sectors verified against the device manifest')
" || fail "sector verify" "a sector's flash content does not match the release"

ok "many_writes ($WRITE_N writes, rx=$WIRE_RX B)"
