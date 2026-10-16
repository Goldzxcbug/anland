#!/usr/bin/env bash
# SteamOS aarch64 Gamescope and Xwayland from current upstream sources.
# Usage: ./build.sh [--nocheck|--noconfirm|--log|--nosign]
# INSTALL=1 explicitly installs; default INSTALL=0, never restarts services.
# BUILD_XWAYLAND=0 builds Gamescope only. JOBS=2 controls parallelism.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ ${1:-} == -h || ${1:-} == --help ]]; then sed -n '2,6p' "$0"; exit 0; fi
for arg in "$@"; do
    case "$arg" in --nocheck|--noconfirm|--log|--nosign) ;; *) echo "Unsupported option: $arg" >&2; exit 1 ;; esac
done
BACKEND="$HERE/gamescope"
XPORT="$(cd "$HERE/../../xwayland/SteamOS_v5" && pwd)"
WORKDIR="${WORKDIR:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-gamescope-build}"
INSTALL="${INSTALL:-${ANLAND_INSTALL:-0}}"
BUILD_XWAYLAND="${BUILD_XWAYLAND:-1}"
for flag in "$INSTALL" "$BUILD_XWAYLAND"; do
    [[ "$flag" == 0 || "$flag" == 1 ]] || { echo 'Flags must be 0 or 1' >&2; exit 1; }
done
export JOBS="${JOBS:-2}"
export MAKEFLAGS="-j$JOBS"
export LANG=C.UTF-8 LC_ALL=C.UTF-8 GIT_TERMINAL_PROMPT=0
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo 'JOBS must be a positive integer' >&2; exit 1; }
[[ $(id -u) != 0 && $(uname -m) == aarch64 ]] || { echo 'Requires an unprivileged aarch64 user' >&2; exit 1; }
[[ -d "$BACKEND" ]] || { echo 'Missing shared backend' >&2; exit 1; }
for tool in makepkg meson ninja tar patch python3 git bsdtar flock sha256sum; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done
[[ -x /usr/bin/steamos-session-shell && -d /usr/share/gamescope/reshade ]] || {
    echo 'Missing SteamOS Gamescope session/ReShade assets' >&2; exit 1;
}
mkdir -p "$WORKDIR" "$HERE/artifacts"
WORKDIR="$(cd "$WORKDIR" && pwd)"
exec 9>"$WORKDIR/.build.lock"
flock -n 9 || { echo 'Another Gamescope build is active' >&2; exit 1; }
RUN="$(mktemp -d "$WORKDIR/build.XXXXXX")"
STAGE="$RUN/stage"
# A fresh per-run source cache resolves each repository's current default branch.
SOURCES="$RUN/sources"
mkdir -p "$STAGE" "$SOURCES"
for name in PKGBUILD gamescope.patch sources.conf startup.sh; do
    cp "$HERE/$name" "$STAGE/"
done
# Follow the canonical shared-source links when preparing makepkg input.
tar -chf "$STAGE/anland-overlay.tar" --exclude='run_*' --exclude='*_test.*' -C "$BACKEND" .
PACKAGES="$HERE/artifacts"
BUILD_PACKAGES="$RUN/packages"
mkdir -p "$BUILD_PACKAGES"
packages=()
build_marker="$RUN/.build-start"
touch "$build_marker"
finish() {
    local rc=$?
    rm -f -- "$build_marker"
    if [[ "$rc" != 0 ]]; then
        printf 'Build failed (rc=%s). Diagnostic workspace: %s\n' "$rc" "$RUN" >&2
    fi
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
collect_packages() {
    local stage="$1" list="$2" pkg metadata name arch
    local before=${#packages[@]}
    (cd "$stage" && PKGDEST="$BUILD_PACKAGES" makepkg --packagelist) > "$list" || return 1
    local -a outputs=()
    mapfile -t outputs < "$list"
    [[ ${#outputs[@]} -gt 0 ]] || { echo 'Empty package list' >&2; return 1; }
    for pkg in "${outputs[@]}"; do
        [[ -s "$pkg" && "$pkg" -nt "$build_marker" ]] || { echo "Missing or stale package: $pkg" >&2; return 1; }
        metadata=$(bsdtar -xOf "$pkg" .PKGINFO) || return 1
        name=$(awk -F' = ' '$1 == "pkgname" {print $2; exit}' <<<"$metadata")
        [[ -n "$name" ]] || { echo "Missing package name: $pkg" >&2; return 1; }
        case "$name" in *-debug|*-dbg|*-dbgsym) continue ;; esac
        arch=$(awk -F' = ' '$1 == "arch" {print $2; exit}' <<<"$metadata")
        [[ "$arch" == aarch64 || "$arch" == any ]] || { echo "Wrong package architecture: $pkg" >&2; return 1; }
        packages+=("$pkg")
    done
    [[ ${#packages[@]} -gt "$before" ]] || { echo 'No binary packages returned' >&2; return 1; }
}
# Build Gamescope first, then build the current Xwayland source if requested.
(
    cd "$STAGE"
    export SRCDEST="$SOURCES" PKGDEST="$BUILD_PACKAGES"
    makepkg --force --cleanbuild --syncdeps --needed "$@"
)
collect_packages "$STAGE" "$RUN/.gamescope-packagelist"
if [[ "$BUILD_XWAYLAND" == 1 ]]; then
    XSTAGE="$RUN/xwayland-package"
    mkdir -p "$XSTAGE" "$RUN/xwayland-sources"
    cp "$XPORT/xorg-xwayland.PKGBUILD" "$XSTAGE/PKGBUILD"
    cp "$XPORT/xwayland.patch" "$XSTAGE/xwayland.patch"
    (
        cd "$XSTAGE"
        export SRCDEST="$RUN/xwayland-sources" PKGDEST="$BUILD_PACKAGES"
        makepkg --force --cleanbuild --syncdeps --needed --nocheck "$@"
    )
    collect_packages "$XSTAGE" "$RUN/.xwayland-packagelist"
fi
delivered=()
for pkg in "${packages[@]}"; do
    bsdtar -tf "$pkg" >/dev/null
    destination="$PACKAGES/${pkg##*/}"
    if [[ -e "$destination.sig" || -L "$destination.sig" ]]; then
        cmp -s "$pkg" "$destination" || { echo "Existing signature preserved; move $destination.sig aside before replacing its package" >&2; exit 1; }
    fi
    cp -p "$pkg" "$destination"
    delivered+=("$destination")
done
sha256sum "${delivered[@]}"
if [[ "$INSTALL" == 1 ]]; then
    sudo pacman -U "${delivered[@]}"
fi
printf 'Build finished (INSTALL=%s). Packages: %s\n' "$INSTALL" "$PACKAGES"
# Keep failed trees for diagnosis; remove disposable sources/objects only after success.
rm -rf -- "$RUN"
