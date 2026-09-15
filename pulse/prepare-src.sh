#!/bin/bash
# prepare-src.sh <dest-dir> [runtime-dir] — pulseaudio source tree for the
# Android (bionic) build: pristine v17.0 from the submodule + the Termux
# patches/modules vendored in pulse/termux/ + the anland tweaks below.
# Called by `make pulse`; the submodule itself is never modified.
#
#   runtime-dir  what @TERMUX_PREFIX@ in fix-paths.patch becomes (the few
#                hard-coded /tmp, machine-id … paths); default = the daemon's
#                default runtime_dir. service.sh sets TMPDIR anyway.
set -euo pipefail
cd "$(dirname "$0")/.."
SRC=third_party/pulseaudio
DST=${1:?dest dir}
RT=${2:-/data/local/tmp/awl}
TERMUX=$PWD/pulse/termux

[ -f "$SRC/meson.build" ] || { echo "prepare-src: $SRC empty — git submodule update --init $SRC" >&2; exit 1; }
rm -rf "$DST"
mkdir -p "$(dirname "$DST")"
cp -r "$SRC" "$DST"
rm -rf "$DST/.git"
DST=$(cd "$DST" && pwd)
cd "$DST"

# meson reads the version from git, or from this file when there is no .git
echo 17.0 > .tarball-version

# Termux (termux-packages packages/pulseaudio): bionic paths, no privilege
# dropping, fd limit for libOpenSLES' dlopen fan-out, sles/aaudio modules in
# the module list, no libintl
for p in fix-paths meson no_priv_drop rlimit-nofile; do
    patch -p1 -s < "$TERMUX/$p.patch"
done
grep -rl '@TERMUX_PREFIX@' src | xargs sed -i "s|@TERMUX_PREFIX@|$RT|g"
mkdir -p src/modules/sles src/modules/aaudio
cp "$TERMUX/module-sles-sink.c" src/modules/sles/
cp "$TERMUX/module-aaudio-sink.c" src/modules/aaudio/

# anland: playback only — the Termux meson patch also lists a sles SOURCE
# (microphone), which we do not ship
sed -i '/module-sles-source/d' src/modules/meson.build

# bionic has no libintl. Termux drops the intl dependency but leaves
# ENABLE_NLS on (their sysroot ships gettext); here NLS follows dgettext
sed -i "s|^cdata.set('ENABLE_NLS', 1)\$|if cc.has_function('dgettext')\n  cdata.set('ENABLE_NLS', 1)\nendif|" meson.build
# no translations: nothing could load them without libintl, and skipping the
# po/ subdir keeps msgfmt off the build host and .mo files out of the module
sed -i "/^ *subdir('po')\$/d" meson.build

grep -q "has_function('dgettext')" meson.build || { echo "prepare-src: NLS sed did not apply" >&2; exit 1; }
echo "pulseaudio source prepared in $DST"
