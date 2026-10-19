#!/usr/bin/env bash
# Runs every case_*.sh in this directory and summarises pass/fail.
#
#   tests/run_all.sh              every case
#   tests/run_all.sh case_lz4_*  a glob, for just the cases you're touching
#
# Each case builds its own fixture binaries and runs against its own
# server+client pair on its own port and scratch dir (see common.sh), so
# cases are independent and this runs them one at a time, in process.
#
# Not covered here: the "flash already holds this content" skip in
# lib/device.c's stage_sector. Under the real protocol the server never
# plans a WRITE for a sector the device's own manifest already matches, so
# that guard has no black-box trigger - it is defence in depth (e.g. against
# a future retry/resend path), not a behaviour these end-to-end cases can
# observe from the outside.
set -eu
cd "$(dirname "$0")"

pattern="${1:-case_*.sh}"
shopt -s nullglob
cases=($pattern)
shopt -u nullglob
[ ${#cases[@]} -gt 0 ] || { echo "no cases match: $pattern" >&2; exit 2; }

pass=0 fail=0

# Shared wire-stats log (tests/common.sh's wire_stats appends to it): clear it
# so this run's grand total doesn't include a previous invocation's requests.
mkdir -p run
: >run/.wire.log

# compress test first, at the geometry of the existing build (so no rebuild flip)
if [ "$pattern" = "case_*.sh" ]; then
    [ -x ../build/cb-flash ] || make -C .. >/dev/null
    eval "$(../build/cb-flash geometry)"
    echo "== compress_test"
    if out="$(make -C .. test-compress ERASE_SIZE="$ERASE_SIZE" SECTOR_SIZE="$SECTOR_SIZE" 2>&1)"; then
        grep -E '^PASS' <<<"$out"
        pass=$((pass + 1))
    else
        echo "$out"
        fail=$((fail + 1))
    fi
fi

for c in "${cases[@]}"; do
    echo "== $c"
    if bash "$c"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
    fi
done

echo
echo "$pass passed, $fail failed"

# Grand total across every update() request this run made: wire bytes,
# framing overhead folded into them, and what was sent against what a full
# raw image transfer per request would have cost.
if [ -s run/.wire.log ]; then
    echo
    awk '
        { tx+=$1; rx+=$2; otx+=$3; orx+=$4; raw+=$5; n++ }
        END {
            printf "wire totals over %d request(s):\n", n
            printf "  tx=%d B (%d B framing, %.1f%%)\n", tx, otx, (tx>0)?100*otx/tx:0
            printf "  rx=%d B (%d B framing, %.1f%%) -- %.1f%% of %d B of raw-image-equivalent traffic\n", \
                rx, orx, (rx>0)?100*orx/rx:0, (raw>0)?100*rx/raw:0, raw
        }
    ' run/.wire.log
fi

[ "$fail" -eq 0 ]
