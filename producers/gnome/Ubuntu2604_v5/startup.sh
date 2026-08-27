#!/bin/bash
# Linux truncates process names to 15 characters in /proc, so cover both forms.
gnome_processes=(gnome-shell gnome-session gnome-session-b gnome-session-s gnome-session-service mutter)
gnome_user_id="$(id -u)"
for process_name in "${gnome_processes[@]}"; do
    pkill -TERM -u "$gnome_user_id" -x "$process_name" > /dev/null 2>&1 || true
done
sleep 1
for process_name in "${gnome_processes[@]}"; do
    pkill -KILL -u "$gnome_user_id" -x "$process_name" > /dev/null 2>&1 || true
done

export ANLAND=1
export ANLAND_SOCKET="${1:-/run/display.sock}"
export QT_QPA_PLATFORM=wayland XDG_CURRENT_DESKTOP=GNOME XDG_SESSION_DESKTOP=gnome XDG_SESSION_TYPE=wayland GNOME_SHELL_SESSION_MODE=gnome
export WAYLAND_DISPLAY=wayland-anland GNOME_WAYLAND_DISPLAY=wayland-anland
export ANLAND_DRM_DEVICE=/dev/dri/renderD128
export MESA_LOADER_DRIVER_OVERRIDE=kgsl TURNIP_KMD=kgsl GALLIUM_DRIVER=freedreno FD_FORCE_KGSL=1
export XDG_RUNTIME_DIR=/run/user/$(id -u)
sudo mkdir -p $XDG_RUNTIME_DIR
sudo chown $(id -un):$(id -gn) $XDG_RUNTIME_DIR
chmod 700 $XDG_RUNTIME_DIR
rm -f $XDG_RUNTIME_DIR/wayland-* > /dev/null 2>&1
sudo mkdir -p /tmp/.X11-unix
sudo chmod 1777 /tmp/.X11-unix
gnome-session
