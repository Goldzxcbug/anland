#!/system/bin/sh
MODDIR=${0%/*}
SOCK=/data/local/tmp/display_daemon.sock
rm -f "$SOCK"
"$MODDIR/display_daemon" "$SOCK" &
