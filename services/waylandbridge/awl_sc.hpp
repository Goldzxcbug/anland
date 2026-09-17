/* awl_sc.hpp — SurfaceControl compositor backend (HWC path)
 *
 * Layer SCs are created straight from the Android window's ANativeWindow
 * (ASurfaceControl_createFromWindow siblings, the device-verified shape):
 * the window Surface is the root of the SC tree and every wayland layer
 * (root surface, subsurfaces, popups, drag icon, cursor) is one sibling in
 * wl stacking order via setZOrder. Frame source = the same per-surface
 * bufferqueue the GL renderer drains.
 *
 * Every layer SC runs in one of three modes, decided per latched frame:
 *   SCANOUT  the client's dma-buf itself, forged into an AHardwareBuffer
 *            (awl_ahb), is latched with setBuffer — zero copy, HWC plane.
 *            Precondition: the frame's pitch is one the display HAL would
 *            compute itself (awl_ahb_hwc_scanout_ok).
 *   EGL      a client dma-buf HWC would refuse is sampled by our GLES
 *            context (forged AHB → EGLImage texture, pitch irrelevant for
 *            the GPU) into a platform-allocated swapchain buffer attached to
 *            the SC — one GPU blit for THIS layer only, the rest of the
 *            window stays zero-copy.
 *   EGL_SHM  wl_shm content is uploaded straight from the client's pool
 *            into a GL texture (damage rect only; the logic layer neither
 *            copies nor queues shm — awl_surface_shm_begin/end) and blitted
 *            like EGL.
 *
 * Ownership split (see awl_sc.cpp header):
 *   - event path (awl_sc_sync, called from window_dirty and attach): layer SC
 *     lifecycle — create when a surface enters the window's stack, retire
 *     (hide + reparent(NULL) + release) when it leaves, setZOrder = stack
 *     index. An SC lives exactly as long as its surface is in the tree.
 *   - render thread (one AChoreographer vsync loop for all windows, owner of
 *     the backend's GLES context): buffer state only — mode decision,
 *     setBuffer / EGL blit + the geometry that depends on that buffer, never
 *     blocking on a client fence.
 *   - OnComplete/OnRelease callbacks (SF threads): self-contained contexts
 *     (frame_done + the release chains of client elements and EGL targets).
 * API floor 31 (device contract): ≤31 calls are unguarded; setFrameTimeline
 * (33) is SDKINT-gated, setBufferWithRelease (36) is dlsym-probed. */
#ifndef AWL_SC_HPP
#define AWL_SC_HPP

#include <android/native_window.h>
#include <stdint.h>

/* attach: record the window, start the global render thread on first use and
 * create the SCs of the window's current stack right away; detach(NULL):
 * retire every layer SC (hide + reparent(NULL)) and hand every in-flight
 * element back. Idempotent — a re-attach first tears down, same semantics as
 * awl_renderer_attach. Any thread. */
int  awl_sc_attach(uint64_t id, ANativeWindow* nw);   /* NULL = detach */

/* Reconcile the window's layer SC set with the logic layer's current stack
 * (create / retire / z). Called by the adapter on every window_dirty — every
 * topology mutation (get_subsurface, place_*, popup, drag icon, cursor,
 * surface death) ends in one — so the SC tree never lags the wayland tree.
 * Cheap when nothing changed. Any thread, no logic-layer lock may be held
 * (takes rwl.rd inside), unknown id = no-op. */
void awl_sc_sync(uint64_t id);

/* window_dirty arrived for this window: ensures the next vsync emits a
 * transaction even when nothing changed visually, so pending frame callbacks
 * ride its OnComplete (GL parity for empty/geometry-only commits). Any
 * thread, returns fast, unknown id = no-op. */
void awl_sc_kick(uint64_t id);

void awl_sc_shutdown(void);

#endif
