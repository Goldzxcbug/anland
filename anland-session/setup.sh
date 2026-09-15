#!/bin/bash
# setupanlandx.sh — install the anland session (session D-Bus + wayland
# link + Xwayland rootless + anland mini-wm) as a systemd USER service for
# the invoking user. Builds mini-wm from source on this machine (the arm64
# Linux container: gcc + libx11-dev + libxcomposite-dev + xwayland), so
# nothing is cross-compiled.
#
#   tar xzf anlandx.tar.gz && bash anlandx/setupanlandx.sh        install/upgrade
#   bash anlandx/setupanlandx.sh --uninstall                       remove
#
# Run as the desktop user (e.g. `su - Gold -c 'bash /path/setupanlandx.sh'`),
# not as root. Installs:
#   ~/.local/bin/anland-miniwm            compiled from miniwm.c
#   ~/.local/bin/anland-session           the session starter
#   ~/.config/systemd/user/anland-session.service enabled + started
#   ~/.config/pulse/client.conf           default-server = the anland host
#                                         PulseAudio (module pulse/), so this
#                                         user's libpulse apps play through
#                                         Android; autospawn off
# While running, ~/.anlandx holds the display, e.g. ":5"
#   → export DISPLAY=$(cat ~/.anlandx)
# and ~/.anlandx-env the app environment (XDG_RUNTIME_DIR=/run/user/<uid>,
# WAYLAND_DISPLAY=wayland-anland, DBUS_SESSION_BUS_ADDRESS).
# Service output: $XDG_RUNTIME_DIR/anland-session.log (journalctl --user is not
# readable for ordinary users in the container).
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

BIN=$HOME/.local/bin
UNIT_DIR=$HOME/.config/systemd/user
PULSE_CLIENT_CONF=$HOME/.config/pulse/client.conf
# host PulseAudio socket, container view (runtime dir convention /run/anland)
PULSE_SOCK=${ANLAND_RUNTIME_DIR:-/run/anland}/pulse.sock
# user bus reachable even from `su -` / droidspaces run (no PAM session env)
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}"

if [ "$(id -u)" = 0 ]; then
    echo "setupanlandx: run as the desktop user, not root (e.g. su - <user> -c 'bash $PWD/setupanlandx.sh')" >&2
    exit 1
fi

# our lines in ~/.config/pulse/client.conf are tagged so install is idempotent and uninstall exact
strip_pulse_conf() {
    [ -f "$PULSE_CLIENT_CONF" ] && sed -i '/^# anland:/d;/^default-server = .*pulse\.sock$/d;/^autospawn = no # anland$/d' "$PULSE_CLIENT_CONF"
}

if [ "${1:-}" = "--uninstall" ]; then
    systemctl --user disable --now anland-session.service anlandx.service 2>/dev/null || true
    rm -f "$UNIT_DIR/anland-session.service" "$UNIT_DIR/anlandx.service" \
          "$BIN/anland-session" "$BIN/anlandx-start" "$BIN/anland-miniwm" \
          "$HOME/.anlandx" "$HOME/.anlandx-env"
    systemctl --user daemon-reload
    strip_pulse_conf
    echo "anland session removed"
    exit 0
fi

if [ ! -d "$XDG_RUNTIME_DIR" ]; then
    echo "setupanlandx: no user runtime dir $XDG_RUNTIME_DIR — log in as $USER once, or: sudo loginctl enable-linger $USER" >&2
    exit 1
fi

missing=()
command -v cc >/dev/null || missing+=(gcc)
[ -f /usr/include/X11/Xlib.h ] || missing+=(libx11-dev)
[ -f /usr/include/X11/extensions/Xcomposite.h ] || missing+=(libxcomposite-dev)
command -v Xwayland >/dev/null || missing+=(xwayland)
if [ ${#missing[@]} -gt 0 ]; then
    echo "setupanlandx: missing ${missing[*]}  →  sudo apt install ${missing[*]}" >&2
    exit 1
fi

mkdir -p "$BIN" "$UNIT_DIR"
cc -O2 -Wall -o "$BIN/anland-miniwm" miniwm.c -lX11 -lXcomposite
install -m 755 anland-session.sh "$BIN/anland-session"
install -m 644 anland-session.service "$UNIT_DIR/anland-session.service"

# upgrade from the old anlandx names
systemctl --user disable --now anlandx.service 2>/dev/null || true
rm -f "$UNIT_DIR/anlandx.service" "$BIN/anlandx-start"

systemctl --user daemon-reload
systemctl --user enable anland-session.service
systemctl --user restart anland-session.service   # (re)start so an upgraded unit/script/binary takes effect now

# sound: this user's libpulse clients (browsers, players, wine…) → the host
# PulseAudio over the runtime-dir socket instead of the container's own server
mkdir -p "$(dirname "$PULSE_CLIENT_CONF")"
strip_pulse_conf
cat >> "$PULSE_CLIENT_CONF" <<EOF
# anland: sound goes to the Android host's PulseAudio (module pulse/, anonymous auth)
default-server = unix:$PULSE_SOCK
autospawn = no # anland
EOF
if [ -S "$PULSE_SOCK" ]; then
    echo "pulse: client.conf → unix:$PULSE_SOCK (socket present)"
else
    echo "pulse: client.conf → unix:$PULSE_SOCK (socket NOT present yet — host daemon module without pulse/, or not started)"
fi

# keep the user manager (and with it this service) alive without an open
# login session; needs root — best effort, the hint is the fallback
if [ "$(loginctl show-user "$USER" -p Linger --value 2>/dev/null)" != "yes" ]; then
    if sudo -n loginctl enable-linger "$USER" 2>/dev/null; then
        echo "linger enabled for $USER"
    else
        echo "note: run 'sudo loginctl enable-linger $USER' so the anland session survives logout"
    fi
fi

sleep 2
systemctl --user --no-pager --lines=5 status anland-session.service || true
if [ -f "$HOME/.anlandx" ]; then
    echo "DISPLAY=$(cat "$HOME/.anlandx")   (from ~/.anlandx)"
else
    echo "~/.anlandx not written yet — follow: tail -f $XDG_RUNTIME_DIR/anland-session.log"
fi
[ -f "$HOME/.anlandx-env" ] && echo "app env:" && cat "$HOME/.anlandx-env"
