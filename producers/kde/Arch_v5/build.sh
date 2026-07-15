#!/bin/bash
#
# build.sh — rebuild patched kwin and Xwayland Arch packages and install them.
#
# Arch counterpart of producers/kde/ubuntu2604_v5/build.sh.  The package
# manager/build commands differ, but the workflow is intentionally the same:
# fetch source, overlay the local backend, apply the patches, build, install.
#
# The KWin backend is shared with Fedora 43 v5 because both use KWin 6.7.2:
#   kwin -> ../anland_backend_fedora43_v5
#
set -euo pipefail

# makepkg refuses to run as root.  If the script was started as
# `sudo bash build.sh`, resume it as the original user so that the build tree,
# package cache, and makepkg metadata belong to that user.  The privileged
# package-install steps below still use sudo normally.
if [ "$(id -u)" -eq 0 ]; then
    if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
        exec sudo -u "$SUDO_USER" -H bash "$0" "$@"
    fi
    printf '\033[1;31m[error] makepkg must not run as root; use a normal user with sudo access (or invoke with sudo so it can drop back automatically).\033[0m\n' >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKDIR="${WORKDIR:-$HOME/anland-archbuild}"
JOBS="${JOBS:-$(nproc)}"

log()  { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m[warn] %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m[error] %s\033[0m\n' "$*" >&2; exit 1; }

find_patch() {
    local name="$1" explicit="${2:-}" candidate
    if [ -n "$explicit" ] && [ -f "$explicit" ]; then
        realpath "$explicit"
        return 0
    fi
    for candidate in "$SCRIPT_DIR/$name" "$PWD/$name"; do
        if [ -f "$candidate" ]; then
            realpath "$candidate"
            return 0
        fi
    done
    return 1
}

find_backend() {
    local candidate
    for candidate in \
        "$SCRIPT_DIR/kwin/src/backends/anland" \
        "$SCRIPT_DIR/kwin" \
        "$SCRIPT_DIR/../anland_backend_fedora43_v5/src/backends/anland" \
        "$SCRIPT_DIR/anland_backend_fedora43_v5/src/backends/anland"; do
        if [ -f "$candidate/CMakeLists.txt" ] && \
           [ -f "$candidate/anland_backend.cpp" ]; then
            realpath "$candidate"
            return 0
        fi
    done
    return 1
}

clone_pkgbuild() {
    local package="$1" pkgdir="$WORKDIR/$package" native_arch
    rm -rf "$pkgdir"
    mkdir -p "$WORKDIR"

    log "Fetching Arch PKGBUILD for '$package'" >&2
    if command -v pkgctl >/dev/null 2>&1; then
        (cd "$WORKDIR" && pkgctl repo clone --protocol=https "$package") >&2 \
            || die "pkgctl could not clone $package"
    else
        git clone --depth=1 \
            "https://gitlab.archlinux.org/archlinux/packaging/packages/$package.git" \
            "$pkgdir" >&2 || die "could not clone the PKGBUILD for $package"
    fi
    [ -f "$pkgdir/PKGBUILD" ] || die "PKGBUILD not found for $package"

    # Arch's official PKGBUILDs commonly declare x86_64 only.  The producer
    # runs in an Arch Linux ARM container, so make the locally built package
    # native to this host before makepkg checks the architecture.  Packages
    # declared as 'any' remain unchanged.
    native_arch="$(uname -m)"
    if grep -q '^arch=' "$pkgdir/PKGBUILD" && \
       ! grep -qE "^arch=.*(['\"]any['\"])" "$pkgdir/PKGBUILD"; then
        sed -Ei "s/^arch=.*/arch=('${native_arch}')/" "$pkgdir/PKGBUILD"
    fi
    printf '%s\n' "$pkgdir"
}

source_tree() {
    local pkgdir="$1" marker="$2" candidate
    while IFS= read -r candidate; do
        if [ -f "$candidate/$marker" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done < <(find "$pkgdir/src" -mindepth 1 -maxdepth 1 -type d | sort)
    die "source tree containing '$marker' not found under $pkgdir/src"
}

apply_once() {
    local tree="$1" patch_file="$2" sentinel="$3"
    if grep -rqF "$sentinel" "$tree" 2>/dev/null; then
        warn "$(basename "$patch_file") is already applied"
        return 0
    fi
    log "Applying $(basename "$patch_file")"
    (cd "$tree" && patch -p1 --forward --reject-file=- < "$patch_file") \
        || die "$(basename "$patch_file") did not apply cleanly"
}

install_local_packages() {
    local package="$1" pacman_config
    shift

    # Some Arch ARM images require signatures even for `pacman -U` local
    # packages. makepkg does not sign unless the user owns a configured GPG
    # key, so use a temporary config that accepts unsigned local files while
    # leaving the system pacman.conf and repository signature policy intact.
    pacman_config="$(mktemp /tmp/anland-pacman.conf.XXXXXX)"
    cp /etc/pacman.conf "$pacman_config"
    sed -i '/^[[:space:]]*LocalFileSigLevel[[:space:]]*=/d' "$pacman_config"
    sed -i '/^\[options\]$/a LocalFileSigLevel = Optional' "$pacman_config"

    if sudo pacman --config "$pacman_config" -U --noconfirm --needed "$@"; then
        rm -f "$pacman_config"
    else
        rm -f "$pacman_config"
        die "pacman could not install the packages built from $package"
    fi
}

build_arch_pkg() {
    local package="$1" tree_marker="$2" patch_file="$3" sentinel="$4" overlay="${5:-}"
    local pkgdir tree cached_dir="$WORKDIR/packages/$package"
    local cached_packages=()

    if [ "${REBUILD:-0}" != 1 ]; then
        mapfile -t cached_packages < <(find "$cached_dir" -maxdepth 1 -type f \
            -name '*.pkg.tar.*' ! -name '*.sig' 2>/dev/null | sort)
        if [ "${#cached_packages[@]}" -gt 0 ]; then
            log "Reusing previously built '$package' packages"
            install_local_packages "$package" "${cached_packages[@]}"
            return 0
        fi
    fi

    pkgdir="$(clone_pkgbuild "$package")"

    log "Downloading and extracting sources for '$package'"
    (cd "$pkgdir" && MAKEFLAGS="-j$JOBS" makepkg --noconfirm --syncdeps \
        --skippgpcheck --nobuild) \
        || die "source preparation failed for $package"
    tree="$(source_tree "$pkgdir" "$tree_marker")"

    if [ -n "$overlay" ]; then
        log "Overlaying the shared KWin 6.7.2 anland backend"
        rm -rf "$tree/src/backends/anland"
        mkdir -p "$tree/src/backends/anland"
        cp -a "$overlay/." "$tree/src/backends/anland/"
    fi

    apply_once "$tree" "$patch_file" "$sentinel"

    log "Building '$package'"
    # --noextract preserves the patched source tree and skips PKGBUILD prepare().
    (cd "$pkgdir" && MAKEFLAGS="-j$JOBS" makepkg --noconfirm --syncdeps \
        --skippgpcheck --noextract) \
        || die "makepkg failed for $package"

    mkdir -p "$WORKDIR/packages/$package"
    find "$pkgdir" -maxdepth 1 -type f -name '*.pkg.tar.*' ! -name '*.sig' \
        -exec cp -f {} "$WORKDIR/packages/$package/" \;

    local packages=()
    mapfile -t packages < <(find "$pkgdir" -maxdepth 1 -type f -name '*.pkg.tar.*' ! -name '*.sig' | sort)
    [ "${#packages[@]}" -gt 0 ] || die "no Arch package produced for $package"

    log "Installing '$package' packages"
    install_local_packages "$package" "${packages[@]}"
}

main() {
    command -v pacman >/dev/null 2>&1 || die "this script must run inside Arch Linux"

    local kwin_patch xwayland_patch backend
    kwin_patch="$(find_patch kwin.patch "${KWIN_PATCH:-}")" \
        || die "kwin.patch not found (set KWIN_PATCH=... to override)"
    xwayland_patch="$(find_patch xwayland.patch "${XWAYLAND_PATCH:-}")" \
        || die "xwayland.patch not found (set XWAYLAND_PATCH=... to override)"
    backend="$(find_backend)" || die "anland backend source not found; copy anland_backend_fedora43_v5/src/backends/anland into $SCRIPT_DIR/kwin"

    log "Installing Arch build tools"
    sudo pacman -Syu --noconfirm --needed base-devel devtools git patch

    log "kwin.patch     : $kwin_patch"
    log "xwayland.patch : $xwayland_patch"
    log "anland backend : $backend"
    log "work directory : $WORKDIR"

    build_arch_pkg kwin 'src/main_wayland.cpp' "$kwin_patch" 'BackendType::Anland' "$backend"
    build_arch_pkg xorg-xwayland 'hw/xwayland/xwayland-glamor-gbm.c' "$xwayland_patch" \
        'No usable linux-dmabuf main device'

    sudo sed -i '/PULSE_SERVER=unix:\/tmp\/.pulse-socket/d' /etc/environment

    log "Done. Patched KWin and Xwayland were built and installed."
    printf 'Built packages: %s\n' "$WORKDIR/packages/{kwin,xorg-xwayland}/"
    printf 'Restart the Plasma Wayland session to use the anland backend.\n'
}

main "$@"
