#!/usr/bin/env bash
# Runs every case_*.sh in this directory and summarises pass/fail.
#
#   tests/run_all.sh              every case
#   tests/run_all.sh case_fill_*  a glob, for just the cases you're touching
#
# Each case builds its own fixture binaries and runs against its own
# server+client pair on its own port and scratch dir (see common.sh), so
# cases are independent and this runs them one at a time, in process.
#
# Not covered here: the "flash already holds this content" skip in
# lib/client.c's flash_worker. Under the real protocol the server never
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
[ "$fail" -eq 0 ]
