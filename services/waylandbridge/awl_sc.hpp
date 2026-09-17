/* awl_sc.hpp — SurfaceControl compositor backend (HWC path)
 *
 * Layer SCs are created straight from the Android window's ANativeWindow
 * (ASurfaceControl_createFromWindow siblings, the device-verified shape):
 * the window Surface is the root of the SC tree and the root WAYLAND layer
 * is simply the first sibling (z=0) — subsurfaces/popup/cursor follow in wl
 * stacking order via setZOrder. The daemon never composites: every layer's
 * queue head is forged into an AHardwareBuffer (awl_ahb) and latched with
 * one ASurfaceTransaction per window per vsync — SurfaceFlinger/HWC does the
 * rest. Frame source = the same per-surface bufferqueue the GL renderer
 * drains.
 *
 * Threading: a global render thread ticks on AChoreographer vsync; a z-worker
 * owns layer-SC lifecycle (create + setZOrder on add/reorder, per the
 * stack-dirty design); OnComplete/OnRelease callbacks arrive on SF threads and
 * only touch self-contained contexts (frame_done + the element release chain).
 * API floor 31 (device contract): ≤31 calls are unguarded; setFrameTimeline
 * (33) is SDKINT-gated, setBufferWithRelease (36) is dlsym-probed. */
#ifndef AWL_SC_HPP
#define AWL_SC_HPP

#include <android/native_window.h>
#include <stdint.h>

/* attach: create the root SC (+ start the global threads on first use);
 * detach(NULL): tear the window's SC tree down and hand every in-flight
 * element back. Idempotent — a re-attach first tears down, same semantics as
 * awl_renderer_attach. Any thread. */
int  awl_sc_attach(uint64_t id, ANativeWindow* nw);   /* NULL = detach */

/* window_dirty arrived for this window (SC mode): ensures the next vsync
 * emits a transaction even when nothing changed visually, so pending frame
 * callbacks ride its OnComplete (GL parity for empty/geometry-only commits).
 * Any thread, returns fast, unknown id = no-op. */
void awl_sc_kick(uint64_t id);

void awl_sc_shutdown(void);

#endif
