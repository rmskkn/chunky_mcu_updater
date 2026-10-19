# Shared setup for the PoC scripts. Sourced, not executed.
set -eu

POC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$POC/build"
RUN="$POC/run"
CACHE="$RUN/cache"
FLASH="$RUN/flash.bin"
IMAGE="$RUN/release.bin"          # the one file the server serves
BASE="$RUN/base.bin"              # the firmware the device shipped with
PORT="${PORT:-7700}"
KLIPPER_BIN="${KLIPPER_BIN:-/home/roman/projects/klipper/out/klipper.bin}"

CB_FLASH="$BUILD/cb-flash"
CB_SERVER="$BUILD/cb-server"
CB_CLIENT="$BUILD/cb-client"

need_build() { [ -x "$CB_FLASH" ] || make -C "$POC" >/dev/null; }

need_reference() {
    [ -f "$KLIPPER_BIN" ] || { echo "reference binary not found: $KLIPPER_BIN" >&2; exit 1; }
}

server_start() {
    "$CB_SERVER" --port "$PORT" --image "$IMAGE" --cache "$CACHE" \
        >"$RUN/server.log" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 50); do
        grep -q listening "$RUN/server.log" 2>/dev/null && return 0
        sleep 0.05
    done
    echo "server did not start:" >&2; cat "$RUN/server.log" >&2; exit 1
}

server_stop() {
    [ -n "${SERVER_PID:-}" ] || return 0
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=
}

update() { "$CB_CLIENT" --flash "$FLASH" --initial "$BASE" --port "$PORT" "$@"; }

rule() { printf '\n== %s\n' "$*"; }
