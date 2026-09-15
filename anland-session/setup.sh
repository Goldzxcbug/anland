#!/bin/bash
# setupanlandx.sh — install the anland session (session D-Bus + wayland
# link + Xwayland rootless + anland mini-wm) as a systemd USER service for
# the invoking user. Builds mini-wm, bwrap and Xwayland from source on this
# machine (the arm64 Linux container), so nothing is cross-compiled.
#
#   tar xzf anlandx.tar.gz && bash anlandx/setupanlandx.sh        install/upgrade
#   bash anlandx/setupanlandx.sh --uninstall                       remove
#
# Run as the desktop user (e.g. `su - Gold -c 'bash /path/setupanlandx.sh'`),
# not as root. Run deps.sh ONCE as root first (apt build dependencies + the
# lfdevs/mesa-for-android-container build for kgsl/turnip — see its header);
# the checks below catch anything still missing. Installs:
#   ~/.local/bin/anland-miniwm            compiled from miniwm.c
#   ~/.local/bin/anland-session           the session starter
#   ~/.local/bin/bwrap                    bubblewrap 0.11.1 + the mountinfo
#                                         index fix (patches/bubblewrap/),
#                                         ALWAYS installed: the distro bwrap
#                                         dies "Out of memory" under glycin's
#                                         RLIMIT_AS on KernelSU+SuSFS devices
#                                         (16 GB id-indexed mount table) → no
#                                         SVG icons → GTK aborts (VS Code dies
#                                         on its file chooser). The fixed
#                                         lookup behaves identically on normal
#                                         kernels, so there is no per-device
#                                         decision and switching kernels can
#                                         never invalidate an installed copy.
#   ~/.local/bin/Xwayland                 patched Xwayland 24.1 (kgsl/turnip
#                                         glamor fixes, patches/xwayland/),
#                                         built with meson from xserver/. The
#                                         system xwayland package is never
#                                         touched — the session resolves
#                                         Xwayland via PATH with ~/.local/bin
#                                         first. Rebuilt only when the
#                                         source/patch input changed
#                                         (xserver/ANLAND-SOURCE); skip with
#                                         ANLAND_XWAYLAND=0.
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
# (returns 0 when the file does not exist — set -e would abort the install otherwise)
strip_pulse_conf() {
    [ -f "$PULSE_CLIENT_CONF" ] || return 0
    sed -i '/^# anland:/d;/^default-server = .*pulse\.sock$/d;/^autospawn = no # anland$/d' "$PULSE_CLIENT_CONF"
}

# is ~/.local/bin/bwrap one of ours? (never touch a user's own binary)
own_bwrap() {
    [ -x "$BIN/bwrap" ] && "$BIN/bwrap" --version 2>/dev/null | grep -q anland
}
remove_own_bwrap() {
    if own_bwrap; then
        rm -f "$BIN/bwrap"
        echo "bwrap: removed $BIN/bwrap (anland build)"
    fi
}

if [ "${1:-}" = "--uninstall" ]; then
    systemctl --user disable --now anland-session.service anlandx.service 2>/dev/null || true
    rm -f "$UNIT_DIR/anland-session.service" "$UNIT_DIR/anlandx.service" \
          "$BIN/anland-session" "$BIN/anlandx-start" "$BIN/anland-miniwm" \
          "$HOME/.anlandx" "$HOME/.anlandx-env"
    remove_own_bwrap
    # patched Xwayland — the stamp file marks it as ours (never touch a user's own)
    if [ -f "$HOME/.anlandx-xwayland" ]; then
        rm -f "$BIN/Xwayland" "$HOME/.anlandx-xwayland"
        echo "xwayland: removed $BIN/Xwayland (anland build)"
    fi
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
[ -f /usr/include/sys/capability.h ] || missing+=(libcap-dev)
if [ ${#missing[@]} -gt 0 ]; then
    echo "setupanlandx: missing ${missing[*]}  →  sudo apt install ${missing[*]}" >&2
    exit 1
fi

mkdir -p "$BIN" "$UNIT_DIR"
cc -O2 -Wall -o "$BIN/anland-miniwm" miniwm.c -lX11 -lXcomposite
install -m 755 anland-session.sh "$BIN/anland-session"
install -m 644 anland-session.service "$UNIT_DIR/anland-session.service"

# bwrap with the mountinfo index fix — installed unconditionally: the fixed
# lookup (sorted array + bsearch) is identical to upstream's table on normal
# kernels, so the same binary is correct everywhere and switching kernels
# can never invalidate it (patches/bubblewrap/README.anland.md). The distro
# bwrap would die "Out of memory" under glycin's RLIMIT_AS on SuSFS devices.
echo "bwrap: building bubblewrap 0.11.1 with the mountinfo index fix"
cc -O2 -Wall -D_GNU_SOURCE -I bubblewrap -o "$BIN/bwrap.new" bubblewrap/*.c -lcap
# self-check under a small address-space limit (glycin sets one of a few GB):
# our build must start a sandbox under it (the distro build cannot, on SuSFS
# fake mount ids). /lib → usr/lib gives the sandbox its dynamic loader (merged /usr).
if (ulimit -v 2000000 && "$BIN/bwrap.new" --unshare-all --ro-bind /usr /usr \
        --symlink usr/lib /lib --dev /dev /usr/bin/true) >/dev/null 2>&1; then
    mv -f "$BIN/bwrap.new" "$BIN/bwrap"
    echo "bwrap: installed $BIN/bwrap ($("$BIN/bwrap" --version))"
else
    rm -f "$BIN/bwrap.new"
    echo "setupanlandx: warning: the fixed bwrap failed its sandbox self-check — not installed; GTK SVG icons will stay broken" >&2
fi

# ---- patched Xwayland (kgsl/turnip glamor fixes) into ~/.local/bin --------
# Built with meson from the vendored xserver/ tree (patch already applied at
# make time). The system xwayland package is never touched — the session
# resolves Xwayland via PATH with ~/.local/bin first (anland-session.sh).
# Rebuilt only when the source/patch input changed (xserver/ANLAND-SOURCE vs
# the ~/.anlandx-xwayland stamp); skip entirely with ANLAND_XWAYLAND=0.
XWL_STAMP=$HOME/.anlandx-xwayland
if [ "${ANLAND_XWAYLAND:-1}" = 0 ]; then
    echo "xwayland: patched build skipped (ANLAND_XWAYLAND=0)"
elif [ -x "$BIN/Xwayland" ] && [ -f "$XWL_STAMP" ] && [ -f xserver/ANLAND-SOURCE ] \
        && [ "$(cat "$XWL_STAMP")" = "$(cat xserver/ANLAND-SOURCE)" ]; then
    echo "xwayland: $BIN/Xwayland already up to date ($(head -1 xserver/ANLAND-SOURCE))"
elif command -v meson >/dev/null && command -v ninja >/dev/null; then
    echo "xwayland: building the patched Xwayland (kgsl/turnip glamor fixes) — this takes a few minutes"
    if ( cd xserver \
         && rm -rf build \
         && meson setup build -Dxvfb=false \
         && meson compile -C build ); then
        install -m 755 xserver/build/hw/xwayland/Xwayland "$BIN/Xwayland"
        cat xserver/ANLAND-SOURCE > "$XWL_STAMP"
        echo "xwayland: installed $BIN/Xwayland ($(head -1 xserver/ANLAND-SOURCE))"
    else
        echo "setupanlandx: warning: the patched Xwayland build failed — keeping the system Xwayland (GL under kgsl may misbehave; see the meson output above)" >&2
    fi
else
    echo "setupanlandx: warning: meson/ninja missing — skipped the patched Xwayland build (system Xwayland stays in use)" >&2
fi

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
