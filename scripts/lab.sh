#!/usr/bin/env bash
# Runtime harness: one server, one client command, no setup step.
#
#   scripts/lab.sh server         run the server; '&' to stay in one terminal
#   scripts/lab.sh release FILE   publish your own firmware binary
#   scripts/lab.sh release [RUNS] or synthesise one: RUNS patched runs (default 4)
#   scripts/lab.sh update         ask the server for it: build and send the
#                                 manifest, take the delta, apply, verify
#   scripts/lab.sh status         where the device stands
#   scripts/lab.sh manifest       inspect the flash
#   scripts/lab.sh map            print this build's flash layout
#   scripts/lab.sh reset          forget the device, so it ships fresh again
#
# The device is created on first use with the reference binary in slot A
# (slot B starts blank), so its first manifest describes real firmware. The
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
    if [ -f "${1:-}" ]; then
        # A real firmware binary: publish it as it is. Naming the release file
        # itself is a no-op rather than an error, so re-publishing what is
        # already there works.
        if [ "$(readlink -f "$1")" = "$(readlink -f "$IMAGE")" ]; then
            echo "$1 is already the release ($(stat -c %s "$IMAGE") B), unchanged"
        else
            cp "$1" "$IMAGE"
            echo "published $1 ($(stat -c %s "$1") B) as the release"
        fi
    else
        # No file given, so synthesise one: the shipped firmware with RUNS
        # patched runs, which is what a release touching a few functions
        # looks like.
        "$CB_FLASH" mutate "$BASE" "$IMAGE" "${1:-4}" "${2:-$RANDOM}"
    fi
    echo "the server re-reads $IMAGE on every request, so no restart is needed"
    ;;
update)
    bootstrap
    exec "$CB_CLIENT" --flash "$FLASH" --initial "$BASE" --port "$PORT" "$@"
    ;;
status)
    exec "$CB_CLIENT" --flash "$FLASH" --initial "$BASE" --status
    ;;
manifest)
    exec "$CB_FLASH" manifest "$FLASH"
    ;;
map)
    exec "$CB_FLASH" map
    ;;
reset)
    rm -f "$FLASH"; rm -rf "$CACHE"
    echo "device forgotten; the next update creates it with $BASE"
    ;;
*)
    sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
    ;;
esac
