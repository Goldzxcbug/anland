#!/bin/bash
# Native path: kwin_wayland IS the top-level compositor, talking to the display
# daemon directly through its built-in "anland" backend (--anland). There is no
# weston layer and no nested kwin — kwin replaces both. The patched kwin_wayland
# must be installed (see kdefix/build.sh, which builds the .deb with the anland
# backend baked in).
SOCK="\${1:-/run/display.sock}"

export XDG_RUNTIME_DIR="\${XDG_RUNTIME_DIR:-/run/user/\$(id -u)}"
mkdir -p "\$XDG_RUNTIME_DIR"
chmod 0700 "\$XDG_RUNTIME_DIR"
unset DISPLAY

# The anland backend reads the daemon socket path from ANLAND_SOCKET
# (falls back to /tmp/display_daemon.sock when unset).
export ANLAND_SOCKET="\$SOCK"

# Native freedreno GL on the kgsl GPU node (loader name "kgsl"). GALLIUM_DRIVER
# pins it so EGL won't silently fall back to zink if freedreno init fails.
export MESA_LOADER_DRIVER_OVERRIDE=kgsl
export GALLIUM_DRIVER=kgsl
export FD_FORCE_KGSL=1
export MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE=1
export MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE_DRI3=1

rm -f "\${XDG_RUNTIME_DIR}"/wayland-* 2>/dev/null

export QT_QPA_PLATFORM=wayland
export FD_MESA_DEBUG=notile
export ANLAND=1
dbus-run-session startplasma-wayland &

