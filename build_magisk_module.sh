#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODULE_DIR="$SCRIPT_DIR/magisk_module"
OUTPUT="$SCRIPT_DIR/anland-daemon.zip"

bash "$SCRIPT_DIR/build_daemon_android.sh"

cp "$SCRIPT_DIR/build_daemon_android/display_daemon" "$MODULE_DIR/"
chmod 755 "$MODULE_DIR/display_daemon"

rm -f "$OUTPUT"
cd "$MODULE_DIR"
zip -r "$OUTPUT" module.prop customize.sh service.sh display_daemon

echo "Magisk module: $OUTPUT"
