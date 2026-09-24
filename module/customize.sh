# customize.sh — sourced by ksud when flashing the zip (env: MODPATH = install
# staging dir, ZIPFILE)

# File modes: the installer has just run `set_perm_recursive $MODPATH 0 0 0755
# 0644` over the whole tree (before sourcing this script) — the zip's modes are
# gone and every binary is 0644. service.sh chmods again at boot, but the
# flashed module should be right on its own (and readable in the manager).
set_perm "$MODPATH/waylandbridge" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755

# KernelSU does not consume a standalone *.anland service-context fragment.
# Build a system overlay from the device's live platform file so
# servicemanager labels anland.host with anland_host_service. Without this
# entry root/su can see the service, but untrusted_app_29 cannot find it.
PLAT_CTX_SRC=/system/etc/selinux/plat_service_contexts
PLAT_CTX_DST="$MODPATH/system/etc/selinux/plat_service_contexts"
if [ -r "$PLAT_CTX_SRC" ] && [ -f "$MODPATH/plat_service_contexts.anland" ]; then
  mkdir -p "${PLAT_CTX_DST%/*}"
  cp -f "$PLAT_CTX_SRC" "$PLAT_CTX_DST"
  if ! grep -q '^anland[.]host[[:space:]]' "$PLAT_CTX_DST"; then
    cat "$MODPATH/plat_service_contexts.anland" >> "$PLAT_CTX_DST"
  fi
  set_perm "$PLAT_CTX_DST" 0 0 0644
  echo "anland: generated platform service-context overlay"
else
  echo "anland: platform service-context source unavailable; anland.host may be hidden from apps"
fi

if [ -d "$MODPATH/pulse/bin" ]; then
  set_perm_recursive "$MODPATH/pulse/bin" 0 0 0755 0755
  echo "anland: pulse/bin marked executable"
fi
