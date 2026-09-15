#!/bin/bash
# anland-session — the per-user anland session inside the Linux container
# (installed by setupanlandx.sh):
#
#   1. session D-Bus at $XDG_RUNTIME_DIR/bus: the system user bus when the
#      systemd user manager provides one, else a dbus-launch'd session bus
#      bound to the same path — apps (glycin/gtk sandboxed SVG loaders,
#      dconf, …) require a bus exactly there
#   2. the anland wayland socket linked into the user runtime dir as
#      wayland-anland — apps run with XDG_RUNTIME_DIR=/run/user/<uid>
#      (writable) and still reach the compositor through the link
#   3. Xwayland -rootless -displayfd → the X server picks the first free
#      display number and reports it over a pipe (no lock-file races)
#   4. that number is published as ":N" in ~/.anlandx (removed on exit) —
#      clients do `export DISPLAY=$(cat ~/.anlandx)`
#   5. mini-wm runs on it (rootless Xwayland surfaces windows only with a
#      WM that redirects them; it also serves the daemon's resize/close
#      channel on $ANLAND_RUNTIME_DIR/anland-wm.sock)
#   6. the app-facing environment is published TWICE: as KEY=VALUE lines in
#      ~/.anlandx-env, and as the systemd user session environment
#      (systemctl --user set-environment) so that apps launched via
#      `systemd-run --user` inherit it and land in app.slice scopes
#   7. when either process exits the other is stopped and the script
#      fails, so the unit's Restart= brings the session back as a whole
#
# Overrides: ~/.config/anlandx.env (sourced) or unit Environment=:
#   ANLAND_RUNTIME_DIR   awl runtime dir as seen from the container. Convention:
#                        /run/anland — droidspaces bind-mounts the host runtime
#                        dir (daemon config "runtime_dir", default
#                        /data/local/tmp/awl) there; wayland-0 lives in it and
#                        the mini-wm socket is created in it, so the host daemon
#                        finds the socket under its own runtime_dir
#   WAYLAND_DISPLAY      socket name in it (default wayland-0; the app-facing
#                        name via the link is always wayland-anland)
#   ANLAND_WM_SOCK       mini-wm control socket (default <runtime dir>/anland-wm.sock)
#   ANLAND_XWAYLAND_ARGS extra Xwayland arguments
#   ANLAND_MINIWM        mini-wm binary (default: anland-miniwm next to this script)
set -u
: "${ANLAND_RUNTIME_DIR:=/run/anland}"
: "${WAYLAND_DISPLAY:=wayland-0}"
[ -r "$HOME/.config/anlandx.env" ] && . "$HOME/.config/anlandx.env"

# user runtime dir (systemd user manager): writable, holds the session bus
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
RT="$XDG_RUNTIME_DIR"
if [ ! -d "$RT" ]; then
    echo "anland-session: no user runtime dir $RT — log in once, or: sudo loginctl enable-linger $USER" >&2
    exit 1
fi

# ---- session D-Bus: system user bus when present, else dbus-launch one at
# the same path (fixed address, so apps find it via $XDG_RUNTIME_DIR/bus)
OUR_DBUS_PID=
if [ -S "$RT/bus" ]; then
    export DBUS_SESSION_BUS_ADDRESS="unix:path=$RT/bus"
else
    DBUS_CONF="$RT/anland-dbus.conf"
    cat > "$DBUS_CONF" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path=$RT/bus</listen>
  <auth>EXTERNAL</auth>
  <standard_session_servicedirs />
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF
    # eval the assignments dbus-launch prints (address + pid)
    eval "$(dbus-launch --sh-syntax --config-file="$DBUS_CONF")"
    OUR_DBUS_PID="${DBUS_SESSION_BUS_PID:-}"
fi
if [ ! -S "$RT/bus" ]; then
    echo "anland-session: session bus did not come up at $RT/bus" >&2
    exit 1
fi

BIN_DIR=$(dirname "$(readlink -f "$0")")
MINIWM=${ANLAND_MINIWM:-$BIN_DIR/anland-miniwm}
STATE=$HOME/.anlandx
ENVF=$HOME/.anlandx-env
WL_LINK=$RT/wayland-anland

cleanup() {
    trap - TERM INT EXIT
    [ -n "${WMPID:-}" ] && kill "$WMPID" 2>/dev/null
    [ -n "${XPID:-}" ] && kill "$XPID" 2>/dev/null
    [ -n "$OUR_DBUS_PID" ] && kill "$OUR_DBUS_PID" 2>/dev/null
    systemctl --user unset-environment DISPLAY WAYLAND_DISPLAY XDG_SESSION_TYPE 2>/dev/null || true
    rm -f "$STATE" "$ENVF" "$WL_LINK"
}
trap 'cleanup; exit 143' TERM INT
trap cleanup EXIT

if [ ! -S "$ANLAND_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then
    echo "anland-session: no wayland socket at $ANLAND_RUNTIME_DIR/$WAYLAND_DISPLAY (anland daemon down?)" >&2
    exit 1
fi
if [ ! -x "$MINIWM" ]; then
    echo "anland-session: $MINIWM missing — run setupanlandx.sh" >&2
    exit 1
fi

# GPU path every other wayland client in this container uses (freedreno over
# kgsl); Xwayland's glamor falls back to software rendering if it fails
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-kgsl}"
export GALLIUM_DRIVER="${GALLIUM_DRIVER:-kgsl}"
export FD_FORCE_KGSL="${FD_FORCE_KGSL:-1}"

# expose the compositor through the user runtime dir; from here on the
# session (Xwayland included) and the apps share one environment
ln -sfn "$ANLAND_RUNTIME_DIR/$WAYLAND_DISPLAY" "$WL_LINK"
export WAYLAND_DISPLAY=wayland-anland
export ANLAND_WM_SOCK="${ANLAND_WM_SOCK:-$ANLAND_RUNTIME_DIR/anland-wm.sock}"
printf 'XDG_RUNTIME_DIR=%s\nWAYLAND_DISPLAY=%s\nDBUS_SESSION_BUS_ADDRESS=%s\n' \
       "$RT" "$WAYLAND_DISPLAY" "${DBUS_SESSION_BUS_ADDRESS:-unix:path=$RT/bus}" > "$ENVF"
# audio: the host PulseAudio socket (module pulse/) — libpulse also reaches it
# via ~/.config/pulse/client.conf (written by setupanlandx.sh); publishing it
# here covers env-driven clients too. Socket is absent when the host daemon
# runs without module pulse/.
if [ -S "$ANLAND_RUNTIME_DIR/pulse.sock" ]; then
    printf 'PULSE_SERVER=unix:%s\n' "$ANLAND_RUNTIME_DIR/pulse.sock" >> "$ENVF"
fi

# display number handshake: Xwayland writes "N\n" to fd 3 once it listens
FIFO=$(mktemp -u "${TMPDIR:-/tmp}/anlandx.XXXXXX")
mkfifo -m 600 "$FIFO" || exit 1
# shellcheck disable=SC2086  # ANLAND_XWAYLAND_ARGS is a word list by design
Xwayland -rootless -noreset -displayfd 3 ${ANLAND_XWAYLAND_ARGS:-} 3>"$FIFO" &
XPID=$!
N=
read -r -t 30 N < "$FIFO"
rm -f "$FIFO"
if [ -z "$N" ]; then
    echo "anland-session: Xwayland reported no display within 30s (see its output above)" >&2
    exit 1
fi

export DISPLAY=":$N"
printf '%s\n' "$DISPLAY" > "$STATE"
echo "anland-session: Xwayland pid $XPID on $DISPLAY → $STATE; wm socket $ANLAND_WM_SOCK"
echo "anland-session: app env → $ENVF (XDG_RUNTIME_DIR=$RT WAYLAND_DISPLAY=$WAYLAND_DISPLAY)"

# publish the anland + mesa environment as the systemd user session
# environment: apps launched with `systemd-run --user` inherit it and run
# in app.slice scopes instead of being orphaned manual children
if ! systemctl --user set-environment \
        "XDG_RUNTIME_DIR=$RT" \
        "WAYLAND_DISPLAY=$WAYLAND_DISPLAY" \
        "DISPLAY=$DISPLAY" \
        "DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-unix:path=$RT/bus}" \
        "XDG_SESSION_TYPE=wayland" \
        "MESA_LOADER_DRIVER_OVERRIDE=$MESA_LOADER_DRIVER_OVERRIDE" \
        "GALLIUM_DRIVER=$GALLIUM_DRIVER" \
        "FD_FORCE_KGSL=$FD_FORCE_KGSL"; then
    echo "anland-session: warning: could not import the session environment (systemctl --user set-environment failed)" >&2
fi
[ -S "$ANLAND_RUNTIME_DIR/pulse.sock" ] && \
    systemctl --user set-environment "PULSE_SERVER=unix:$ANLAND_RUNTIME_DIR/pulse.sock" || true

"$MINIWM" &
WMPID=$!

# first one to exit takes the session down (bash ≥ 5.1 wait -n with pids)
wait -n "$XPID" "$WMPID"
rc=$?
echo "anland-session: Xwayland or mini-wm exited (rc=$rc) — stopping the session" >&2
exit 1
