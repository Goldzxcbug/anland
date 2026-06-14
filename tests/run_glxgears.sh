#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="${SCRIPT_DIR}/.."
BUILD_DIR="${ROOT_DIR}/build"
WESTON_BUILDDIR="${ROOT_DIR}/weston/builddir"
SOCK="/tmp/display_daemon.sock"
WESTON_SOCK="wayland-anland"
DURATION="${1:-15}"

WESTON_BIN="${WESTON_BUILDDIR}/frontend/weston"
WESTON_MODULE_MAP="color-lcms.so=${WESTON_BUILDDIR}/libweston/color-lcms/color-lcms.so;gl-renderer.so=${WESTON_BUILDDIR}/libweston/renderer-gl/gl-renderer.so;anland-backend.so=${WESTON_BUILDDIR}/libweston/backend-anland/anland-backend.so;xwayland.so=${WESTON_BUILDDIR}/xwayland/xwayland.so;desktop-shell.so=${WESTON_BUILDDIR}/desktop-shell/desktop-shell.so;weston-keyboard=${WESTON_BUILDDIR}/clients/weston-keyboard;weston-desktop-shell=${WESTON_BUILDDIR}/clients/weston-desktop-shell;"

cleanup() {
    echo "--- cleanup ---"
    [ -n "$GLXGEARS_PID" ] && kill "$GLXGEARS_PID" 2>/dev/null
    [ -n "$WESTON_PID" ]   && kill "$WESTON_PID"   2>/dev/null
    [ -n "$CONSUMER_PID" ] && kill "$CONSUMER_PID"  2>/dev/null
    [ -n "$DAEMON_PID" ]   && kill "$DAEMON_PID"    2>/dev/null
    sleep 0.3
    # force kill stragglers
    [ -n "$GLXGEARS_PID" ] && kill -9 "$GLXGEARS_PID" 2>/dev/null
    [ -n "$WESTON_PID" ]   && kill -9 "$WESTON_PID"   2>/dev/null
    [ -n "$CONSUMER_PID" ] && kill -9 "$CONSUMER_PID"  2>/dev/null
    [ -n "$DAEMON_PID" ]   && kill -9 "$DAEMON_PID"    2>/dev/null
    wait 2>/dev/null
    rm -f "$SOCK"
    rm -f "/tmp/${WESTON_SOCK}"
    rm -f "/tmp/${WESTON_SOCK}.lock"
}
trap cleanup EXIT

# kill any leftover processes from previous runs
echo "=== Killing leftover processes ==="
pkill -f "display_daemon.*${SOCK}" 2>/dev/null || true
pkill -f "test_sdl_consumer" 2>/dev/null || true
pkill -f "weston.*anland" 2>/dev/null || true
sleep 0.5
rm -f "$SOCK"

# build cmake project
echo "=== Building cmake project ==="
cmake -B "${BUILD_DIR}" -S "${ROOT_DIR}" > /dev/null 2>&1
cmake --build "${BUILD_DIR}" -j"$(nproc)" 2>&1 | tail -5

# build weston
echo "=== Building weston ==="
ninja -C "${WESTON_BUILDDIR}" 2>&1 | tail -5

# 1. start daemon
echo "=== Starting daemon ==="
"${BUILD_DIR}/display_daemon" "$SOCK" &
DAEMON_PID=$!
sleep 0.3
echo "daemon PID=$DAEMON_PID"

# 2. start SDL consumer (refresh=0 unlimited)
echo "=== Starting SDL consumer ==="
LD_LIBRARY_PATH="${BUILD_DIR}" "${BUILD_DIR}/test_sdl_consumer" \
    --socket "$SOCK" --width 1280 --height 720 --gpu /dev/dri/renderD128 &
CONSUMER_PID=$!
sleep 0.5
echo "consumer PID=$CONSUMER_PID"

# 3. snapshot existing X sockets before weston starts
EXISTING_X=$(ls /tmp/.X11-unix/ 2>/dev/null || true)

# start weston with anland backend + xwayland
echo "=== Starting weston ==="
export WESTON_MODULE_MAP
export LD_LIBRARY_PATH="${BUILD_DIR}:${WESTON_BUILDDIR}/libweston:${LD_LIBRARY_PATH}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp}"

"${WESTON_BIN}" \
    --backend=anland \
    --socket="${WESTON_SOCK}" \
    --width=1280 --height=720 \
    --socket-path="$SOCK" \
    --xwayland \
    --no-xwm-decorations \
    2>&1 | sed 's/^/[weston] /' &
WESTON_PID=$!
echo "weston PID=$WESTON_PID"

# 4. wait for Xwayland — find the NEW X socket that weston spawned
echo "=== Waiting for Xwayland ==="
DISPLAY_NUM=""
for attempt in $(seq 1 30); do
    for xsock in /tmp/.X11-unix/X*; do
        [ -S "$xsock" ] || continue
        num="${xsock##*/tmp/.X11-unix/X}"
        # skip sockets that existed before weston
        echo "$EXISTING_X" | grep -qw "X${num}" && continue
        if DISPLAY=":${num}" xdpyinfo >/dev/null 2>&1; then
            DISPLAY_NUM="${num}"
            break 2
        fi
    done
    sleep 0.3
done

if [ -z "$DISPLAY_NUM" ]; then
    echo "ERROR: Xwayland not found"
    exit 1
fi
echo "Xwayland ready on :${DISPLAY_NUM}"

# 5. launch glxgears
echo "=== Launching glxgears (${DURATION}s) ==="
DISPLAY=":${DISPLAY_NUM}" vblank_mode=0 glxgears 2>&1 | sed 's/^/[glxgears] /' &
GLXGEARS_PID=$!
echo "glxgears PID=$GLXGEARS_PID"

# run for DURATION seconds
sleep "$DURATION"

echo "=== Done ==="
