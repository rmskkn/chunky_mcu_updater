#!/usr/bin/env bash
# Runs the suite at each ERASE:SECTOR geometry (a clean rebuild per entry),
# then restores the default build.
set -u
cd "$(dirname "$0")/.."

configs="1024:1024 1024:2048 2048:2048 1024:4096"
rc=0
summary=()

for c in $configs; do
    e=${c%%:*}; s=${c##*:}
    echo "=== ERASE_SIZE=$e SECTOR_SIZE=$s"
    if ! make clean all ERASE_SIZE="$e" SECTOR_SIZE="$s" >/dev/null; then
        summary+=("$c: build failed"); rc=1; continue
    fi
    out="$(tests/run_all.sh 2>&1)"; st=$?
    echo "$out" | grep -E '^(FAIL|[0-9]+ passed)'
    summary+=("$c: $(echo "$out" | grep -E '^[0-9]+ passed')")
    [ $st -eq 0 ] || rc=1
done

echo
echo "=== matrix summary"
printf '%s\n' "${summary[@]}"

make clean all >/dev/null
exit $rc
