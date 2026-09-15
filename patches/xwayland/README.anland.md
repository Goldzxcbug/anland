# Xwayland 24.1 (vendored, one patch) — why anland builds its own Xwayland

Source: `third_party/xserver` pinned on the `xwayland-24.1` branch
(v24.1.10, the standalone Xwayland tree). The submodule stays pristine; this
patch is applied to the staged copy by `make anlandx` (same scheme as
`patches/bubblewrap/` and `pulse/termux/`). Carried over from the legacy
producer (`producers/kde/ubuntu2604_v5/xwayland.patch`), where it was
applied to the distro source package and rebuilt as a .deb — now it is
applied to the vendored tree instead, so the system xwayland package is
never touched.

## The bugs it fixes (kgsl/turnip stack)

On the Qualcomm GPU exposed through kgsl (freedreno + turnip mesa, the
on-device stack), stock Xwayland 24.1 cannot do accelerated GL for X11 apps:

1. **Black X11 windows.** An implicit-modifier buffer (`DRM_FORMAT_MOD_INVALID`)
   is never listed in the compositor's advertised modifier set, so the regular
   support check fails. On kgsl the buffer is allocated on the display node
   but composited by turnip, which left both the dmabuf and the (absent)
   wl_drm path unusable → no wl_buffer → the window stayed black. Per the
   linux-dmabuf protocol INVALID means "let the compositor infer the
   layout", so the patch submits it through the dmabuf path directly.
2. **BadAlloc / segfault in dri3.** Some freedreno/kgsl mesa builds rewrite a
   LINEAR modifier (0) to a bogus 1274 (0x4FA) in the `loader_dri3`
   `pixmap_from_buffers` path; 1274 is not a valid DRM modifier, so
   `gbm_bo_import()` rejects it and the X11 GL client dies. The buffer is
   really linear — the patch maps 1274 back to LINEAR.
3. **Startup hang / NULL-main_dev crash.** linux-dmabuf feedback (and with it
   a main device) only exists from protocol version 4; the anland compositor
   speaks v3, so stock Xwayland either blocked forever on `feedback_done` or
   dereferenced a NULL main_dev (the GPU lives behind `/dev/kgsl-3d0`, not a
   DRM render node). The patch skips the feedback wait below v4 and falls
   back to a render node (`XWAYLAND_GBM_DEVICE`, default
   `/dev/dri/renderD128`) which the patched freedreno mesa redirects to kgsl.
4. **glamor refused entirely.** With neither wl_drm nor v4 feedback, stock
   `xwl_glamor_check_wl_drm()` disabled glamor; v3 dmabuf is enough to bring
   it up (fixes 1–3 then carry the buffers), so the patch keeps glamor
   available whenever linux-dmabuf v3 is present.

## How it is deployed

`make anlandx` stages the submodule's tracked files (with the exact input —
commit + patch checksum — recorded in `xserver/ANLAND-SOURCE`), applies this
patch, and ships the patched tree in the tarball. `setupanlandx.sh` builds
it with meson (`deps.sh` installs the build dependencies first; on
Debian/Ubuntu via `apt-get build-dep xwayland`, the distro's own dep list —
no `apt source` involved) and installs the single binary as
`~/.local/bin/Xwayland`. The system xwayland package stays untouched;
`anland-session.sh` puts `~/.local/bin` first in PATH, so the session simply
resolves `Xwayland` to the patched build when present (and falls back to the
distro binary if the build was skipped). `setupanlandx.sh --uninstall`
removes it again (stamp file `~/.anlandx-xwayland` marks it as ours).

Worth sending upstream after re-checking against `xserver` main.
