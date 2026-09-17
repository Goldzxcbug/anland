/* awl_renderer.hpp — GPU renderer (per-window EGL) */
#ifndef AWL_RENDERER_HPP
#define AWL_RENDERER_HPP

#include "awl.h"
#include "awl_geom.h"   /* snap/sampled-extent/fourcc helpers (shared with awl_sc) */

#include <android/native_window.h>
#include <stdint.h>

/* attach: create GL resources (calling thread, concurrency-safe) + spawn the
 * window's dedicated render thread; detach(NULL): stop+join the render thread
 * then release GL — strictly ordered with that window's rendering */
int  awl_renderer_attach(uint64_t id, ANativeWindow* nw);   /* NULL=detach */

/* Request a render (any thread, returns fast): sets the window's render
 * request and wakes its render thread. The actual GL runs on each window's
 * own thread — windows render in parallel, a frame submit only blocks itself */
void awl_renderer_request_render(uint64_t id);

void awl_renderer_shutdown(void);

#endif
