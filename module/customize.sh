# customize.sh — sourced by ksud when flashing the zip (env: MODPATH = install
# staging dir, ZIPFILE)

# File modes: the installer has just run `set_perm_recursive $MODPATH 0 0 0755
# 0644` over the whole tree (before sourcing this script) — the zip's modes are
# gone and every binary is 0644. service.sh chmods again at boot, but the
# flashed module should be right on its own (and readable in the manager).
set_perm "$MODPATH/waylandbridge" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
if [ -d "$MODPATH/pulse/bin" ]; then
  set_perm_recursive "$MODPATH/pulse/bin" 0 0 0755 0755
  echo "anland: pulse/bin marked executable"
fi
