#!/usr/bin/env bash
# Build patched KWin and Xwayland from current upstream default branches.
# INSTALL=1 explicitly installs the resulting packages; default is build-only.
# BUILD_XWAYLAND=0 skips the Xwayland package.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XPORT="$(cd "$HERE/../../xwayland/SteamOS_v5" && pwd)"
BACKEND="$HERE/kwin/src/backends/anland"
WORKDIR="${WORKDIR:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-kwin-latest/package}"
INSTALL="${INSTALL:-${ANLAND_INSTALL:-0}}"
BUILD_XWAYLAND="${BUILD_XWAYLAND:-1}"
JOBS="${JOBS:-2}"
[[ "$INSTALL" == 0 || "$INSTALL" == 1 ]] || { echo 'INSTALL must be 0 or 1' >&2; exit 1; }
[[ "$BUILD_XWAYLAND" == 0 || "$BUILD_XWAYLAND" == 1 ]] || { echo 'BUILD_XWAYLAND must be 0 or 1' >&2; exit 1; }
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo 'JOBS must be a positive integer' >&2; exit 1; }
[[ $(id -u) != 0 && $(uname -m) == aarch64 ]] || { echo 'Requires an unprivileged aarch64 user' >&2; exit 1; }
[[ -d "$BACKEND" ]] || { echo 'Missing KWin Anland backend source' >&2; exit 1; }
for tool in makepkg tar patch python3 bsdtar sha256sum; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done
[[ $(qmake6 -query QT_VERSION) == 6.8.0 ]] || {
    echo 'Requires the SteamOS Qt 6.8.0 build environment' >&2; exit 1;
}
mkdir -p "$WORKDIR/stage" "$WORKDIR/xwayland-package" "$WORKDIR/srcdest" \
    "$WORKDIR/xwayland-srcdest" "$WORKDIR/packages"
WORKDIR="$(cd "$WORKDIR" && pwd)"
exec 9>"$WORKDIR/.build.lock"
flock -n 9 || { echo 'Another KWin build is active' >&2; exit 1; }

backend_manifest() {
    python3 - "$BACKEND" <<'PY_MANIFEST'
from pathlib import Path
import hashlib, os, sys
root=Path(sys.argv[1]); entries=[]
for directory,dirs,files in os.walk(root,followlinks=True):
    dirs.sort()
    for name in sorted(files):
        p=Path(directory)/name
        entries.append((p.relative_to(root).as_posix(),hashlib.sha256(p.read_bytes()).hexdigest()))
for name,digest in sorted(entries): print(digest+'  '+name)
PY_MANIFEST
}
install_packages() {
    cp /etc/pacman.conf "$WORKDIR/pacman-local.conf"
    if grep -q '^#LocalFileSigLevel = Optional$' "$WORKDIR/pacman-local.conf"; then
        sed -i 's/^#LocalFileSigLevel = Optional$/LocalFileSigLevel = Optional/' "$WORKDIR/pacman-local.conf"
    else
        printf '\n[options]\nLocalFileSigLevel = Optional\n' >> "$WORKDIR/pacman-local.conf"
    fi
    sudo pacman --config "$WORKDIR/pacman-local.conf" -U --noconfirm "$@"
}
packages=()
collect_packages() {
    local stage="$1" list="$2" pkg
    (cd "$stage" && PKGDEST="$WORKDIR/packages" makepkg --packagelist) > "$list"
    while IFS= read -r pkg; do
        [[ -s "$pkg" ]] || { echo "Missing package: $pkg" >&2; return 1; }
        bsdtar -tf "$pkg" >/dev/null
        packages+=("$pkg")
    done < "$list"
    [[ -s "$list" ]] || { echo 'Empty package list' >&2; return 1; }
}

if [[ "$BUILD_XWAYLAND" == 1 ]]; then
    XSTAGE="$WORKDIR/xwayland-package"
    cp "$XPORT/xorg-xwayland.PKGBUILD" "$XSTAGE/PKGBUILD"
    cp "$XPORT/xwayland.patch" "$XSTAGE/xwayland.patch"
    (cd "$XSTAGE" && SRCDEST="$WORKDIR/xwayland-srcdest" PKGDEST="$WORKDIR/packages" JOBS="$JOBS" \
        makepkg --force --cleanbuild --syncdeps --nocheck)
    collect_packages "$XSTAGE" "$WORKDIR/xwayland-packagelist"
fi

STAGE="$WORKDIR/stage"
cp "$HERE/PKGBUILD" "$HERE/kwin.patch" "$STAGE/"
backend_manifest > "$STAGE/.backend.sha256"
mkdir -p "$WORKDIR/overlay/anland"
cp -aL "$HERE/kwin/src/backends/anland/." "$WORKDIR/overlay/anland/"
tar -chf "$STAGE/anland-overlay.tar" -C "$WORKDIR/overlay" anland
(cd "$STAGE" && SRCDEST="$WORKDIR/srcdest" PKGDEST="$WORKDIR/packages" JOBS="$JOBS" \
    makepkg --force --cleanbuild --syncdeps --nocheck)
collect_packages "$STAGE" "$WORKDIR/kwin-packagelist"
for pkg in "${packages[@]}"; do sha256sum "$pkg"; done
if [[ "$INSTALL" == 1 ]]; then install_packages "${packages[@]}"; fi
printf 'Build finished from current upstream sources (INSTALL=%s). Packages: %s\n' "$INSTALL" "$WORKDIR/packages"
