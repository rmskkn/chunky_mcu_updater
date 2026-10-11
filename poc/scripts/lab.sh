#!/usr/bin/env bash
# Runtime harness: one server, one client command, no setup step.
#
#   scripts/lab.sh server         run the server; '&' to stay in one terminal
#   scripts/lab.sh release [RUNS] publish a new release, RUNS patched runs (default 4)
#   scripts/lab.sh update         one whole update attempt
#   scripts/lab.sh status         where the device stands
#   scripts/lab.sh manifest|map   inspect the flash
#   scripts/lab.sh reset          forget the device, so it ships fresh again
#
# The device is created on first use with the reference binary in its one
# application partition, so its first manifest describes real firmware. The
# server stays up across requests and prints one stats row per request.
source "$(dirname "$0")/common.sh"
need_build

# Everything needs the shipped firmware and something to serve.
bootstrap() {
    need_reference
    mkdir -p "$CACHE"
    [ -f "$BASE" ]  || cp "$KLIPPER_BIN" "$BASE"
    [ -f "$IMAGE" ] || cp "$BASE" "$IMAGE"
}

cmd="${1:-help}"; shift || true

case "$cmd" in
server)
    bootstrap
    exec "$CB_SERVER" --port "$PORT" --image "$IMAGE" --cache "$CACHE" "$@"
    ;;
release)
    bootstrap
    "$CB_FLASH" mutate "$BASE" "$IMAGE" "${1:-4}" "${2:-$RANDOM}"
    echo "published $IMAGE; the server picks it up on the next update"
    ;;
update)
    bootstrap
    exec "$CB_CLIENT" --flash "$FLASH" --initial "$BASE" --port "$PORT" "$@"
    ;;
status)
    exec "$CB_CLIENT" --flash "$FLASH" --initial "$BASE" --status
    ;;
manifest|map)
    exec "$CB_FLASH" "$cmd" "$FLASH"
    ;;
reset)
    rm -f "$FLASH"; rm -rf "$CACHE"
    echo "device forgotten; the next update creates it with $BASE"
    ;;
*)
    sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
    ;;
esac
