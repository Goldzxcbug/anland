#!/bin/bash
# deps.sh — root-side dependency setup for the anlandx session components
# (mini-wm, patched bwrap, patched Xwayland). Run ONCE as ROOT inside the
# container, BEFORE the desktop user runs setupanlandx.sh:
#
#   bash /path/to/anlandx/deps.sh                              interactive
#   bash /path/to/anlandx/deps.sh --nointeractive --mesa URL  unattended
#   … then, as the desktop user:
#   bash /path/to/anlandx/setupanlandx.sh
#
# It is separate from setupanlandx.sh on purpose: everything here needs root
# (apt, overwriting files under /usr), while setupanlandx.sh must run as the
# desktop user. Idempotent.
#
# What it does:
#   1. apt build dependencies (Debian/Ubuntu): the base set plus the distro's
#      own build-dep sets for bubblewrap and xwayland (`apt-get build-dep`) —
#      the -dev versions always match the installed libraries regardless of
#      release. deb-src entries are index metadata only: no distro source
#      code is fetched, and no system package is replaced by this part.
#   2. Mesa: install the distro mesa packages (the file layout the Android
#      mesa build overwrites), then extract the mesa tarball given by
#      --mesa / the prompt over / — freedreno/kgsl/turnip for the Adreno GPU
#      (the distro mesa has no kgsl backend). The URL is NEVER guessed: pick
#      the asset matching your distro yourself from
#      https://github.com/lfdevs/mesa-for-android-container/releases
#
# Options:
#   --mesa URL        mesa tarball URL (from the releases page above)
#   --nointeractive   unattended: DEBIAN_FRONTEND=noninteractive; requires
#                     --mesa (nothing can be prompted)
#   With no options the script asks for the mesa URL on the command line
#   and lets apt ask its configuration questions (tzdata etc.).
set -u

usage() {
    sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'
}

MESA_URL=
NOINTERACTIVE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --nointeractive) NOINTERACTIVE=1 ;;
        --mesa)    shift; [ $# -gt 0 ] || { echo "deps: --mesa needs a URL" >&2; exit 1; }; MESA_URL=$1 ;;
        --mesa=*)  MESA_URL=${1#--mesa=} ;;
        -h|--help) usage; exit 0 ;;
        *)         echo "deps: unknown option: $1 (see --help)" >&2; exit 1 ;;
    esac
    shift
done

if [ "$(id -u)" -ne 0 ]; then
    echo "deps: must run as root (apt and the mesa overwrite need it)" >&2
    exit 1
fi

if [ "$NOINTERACTIVE" = 1 ]; then
    # unattended: nothing may prompt — the mesa URL must already be there
    [ -n "$MESA_URL" ] || { echo "deps: --nointeractive requires --mesa <url>" >&2; exit 1; }
    export DEBIAN_FRONTEND=noninteractive
fi

# ---- mesa tarball URL (never guessed — the user picks the distro asset) ----
if [ -z "$MESA_URL" ]; then
    echo "mesa tarball for this container (freedreno/kgsl/turnip):"
    echo "  pick the asset matching your distro from"
    echo "  https://github.com/lfdevs/mesa-for-android-container/releases"
    printf 'URL: '
    read -r MESA_URL
fi
[ -n "$MESA_URL" ] || { echo "deps: no mesa URL given — nothing installed" >&2; exit 1; }

. /etc/os-release 2>/dev/null || true

case " ${ID:-unknown} ${ID_LIKE:-} " in
    *"debian"*|*"ubuntu"*) ;;
    *)
        echo "deps: distribution '${PRETTY_NAME:-${ID:-unknown}}' is not handled automatically — install by hand:" >&2
        echo "  gcc, X11/Xcomposite development headers, meson, ninja, pkg-config," >&2
        echo "  xwayland (runtime fallback), the bubblewrap + Xwayland build dependencies" >&2
        echo "  (Debian/Ubuntu equivalent: apt-get build-dep bubblewrap xwayland)," >&2
        echo "  and the mesa tarball ($MESA_URL) extracted over /" >&2
        exit 1
        ;;
esac

# ---- apt: build dependencies ----------------------------------------------
# build-dep reads Build-Depends from the source index → deb-src entries are
# required (apt-get update then fetches the Sources index too)
if ! grep -rqsE '^Types:.*deb-src|^deb-src ' /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null; then
    echo "deps: enabling deb-src index entries (for build-dep only)"
    if [ -f /etc/apt/sources.list.d/ubuntu.sources ]; then
        sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources
    elif [ -f /etc/apt/sources.list ]; then
        sed -i 's/^deb \(.*\)$/deb \1\ndeb-src \1/' /etc/apt/sources.list
    fi
fi
apt-get update -qq || echo "deps: apt-get update reported issues" >&2

# base set: gcc, the mini-wm headers (libx11, libxcomposite), build tools,
# curl (the mesa download below), the distro xwayland as the runtime
# fallback, libpam-systemd (without pam_systemd.so the systemd --user
# manager exits "XDG_RUNTIME_DIR is not set" — the session needs it), and
# the distro mesa packages — the file layout the Android mesa build
# overwrites (EGL/GLX/gbm/gallium/dri + turnip vulkan). Package names
# drift between releases and distros (e.g. libglapi-mesa0 exists on Debian,
# not on Ubuntu noble), so a failed bulk install is retried per package,
# skipping with a warning whatever this distro does not have.
BASE_PKGS="gcc libx11-dev libxcomposite-dev meson ninja-build pkg-config curl xwayland \
    libpam-systemd \
    libegl-mesa0 libglx-mesa0 libgbm1 libgl1-mesa-dri mesa-vulkan-drivers \
    libegl1 libgl1 libglx0 mesa-utils"
if ! apt-get install -y $BASE_PKGS; then
    echo "deps: bulk install failed — retrying package by package" >&2
    for p in $BASE_PKGS; do
        apt-get install -y "$p" \
            || echo "deps: warning: '$p' not available on this distro — skipped" >&2
    done
fi

# the full build-dep sets of the two vendored components, as this distro
# defines them (bwrap: libcap etc.; Xwayland: the xserver stack). Separate
# invocations: one unknown source-package name must not take the other down.
apt-get build-dep -y bubblewrap \
    || echo "deps: build-dep bubblewrap failed" >&2
apt-get build-dep -y xwayland \
    || echo "deps: build-dep xwayland failed (older releases may name the source package differently — the Xwayland build then needs its -dev packages by hand)" >&2

# ---- mesa: the Android container build (freedreno/kgsl/turnip) -------------
# The given tarball extracted over / (they ship ./usr/… files only: dri
# drivers incl. kgsl_dri.so, libEGL_mesa/libGLX_mesa/libgbm/libgallium, the
# turnip vulkan driver, glvnd/vulkan json, drirc).
echo "deps: mesa ← $MESA_URL"
if curl -fsSL "$MESA_URL" -o /tmp/anland-mesa.tgz; then
    tar -xf /tmp/anland-mesa.tgz -C /        # overwrites the distro mesa files
    rm -f /tmp/anland-mesa.tgz
    echo "deps: mesa installed ($(basename "$MESA_URL"))"
else
    echo "deps: error: could not download the mesa tarball — nothing overwritten" >&2
    exit 1
fi
