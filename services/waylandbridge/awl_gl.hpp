/* awl_gl.hpp — process-wide EGL/GLES glue shared by the two composition
 * backends: the GL compositor (awl_renderer.cpp, one window surface + context
 * per window) and the SurfaceControl backend's EGL-mode layers (awl_sc.cpp,
 * one offscreen context on its vsync thread blitting into platform-allocated
 * buffers). One EGLDisplay, one config, one set of extension entry points,
 * one layer-quad program and one AHardwareBuffer → texture import path — the
 * two backends must sample a client buffer identically (channel order,
 * transform matrix, pixel-grid snap of the dst), so the code is shared, not
 * mirrored. */
#ifndef AWL_GL_HPP
#define AWL_GL_HPP

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>   /* GL_BGRA_EXT etc. (gl3.h must come first for base types) */
#include <android/hardware_buffer.h>
#include <stdint.h>

struct awl_ahb_slot;

/* ---- display ----
 * Idempotent, thread-safe; a failure is retried by the next call (no
 * call_once: a transient failure there would lock rendering up forever).
 * Config: ES3, RGB888, window + pbuffer capable. */
bool awl_gl_init(void);
EGLDisplay awl_gl_display(void);
EGLConfig awl_gl_config(void);
bool awl_gl_has_native_fence(void);   /* EGL_ANDROID_native_fence_sync */

/* ES3 context (nothing shared between contexts). EGL_NO_CONTEXT = failed. */
EGLContext awl_gl_create_context(void);
/* Bind `ctx` to the calling thread without a window: surfaceless when
 * EGL_KHR_surfaceless_context is exposed, else on a 1x1 pbuffer returned in
 * *pbuf (EGL_NO_SURFACE when surfaceless; the caller destroys it after
 * unbinding). */
bool awl_gl_make_current_offscreen(EGLContext ctx, EGLSurface* pbuf);

/* ---- layer quad ----
 * One textured quad per layer: dst rect in view pixels (Y down), sample
 * sub-rect (viewport source, normalized), wl_surface.set_buffer_transform
 * as a uv matrix. u_flip = +1 renders into a window surface (GL's origin is
 * the window's bottom-left, so Y is mirrored to keep "Y down"), −1 into an
 * FBO whose texture is handed to SurfaceFlinger (row 0 = top). */
struct awl_gl_quad {
    GLuint program = 0, vbo = 0;
    GLint a_pos = -1, u_view = -1, u_dst = -1, u_uv = -1, u_xform = -1,
          u_flip = -1, u_tex = -1;
};
bool awl_gl_quad_create(awl_gl_quad* q);   /* on the current context */
void awl_gl_quad_destroy(awl_gl_quad* q);
/* Bind program + vbo + texture unit 0, set the per-target constants. */
void awl_gl_quad_begin(const awl_gl_quad* q, float view_w, float view_h, float flip);
/* Draw `tex` into dst = {x, y, w, h} (view px) sampling uv = {u0, v0, su, sv}
 * through wl transform 0..7. Blend state is the caller's. */
void awl_gl_quad_draw(const awl_gl_quad* q, GLuint tex, const float dst[4],
                      const float uv[4], int transform);

/* wl buffer transform (wl_output.transform 0..7) → sample-uv affine for a
 * layer quad: rows u = a·qx + b·qy + c, v = d·qx + e·qy + f (q = top-down
 * display uv; the buffer holds the content rotated by T, we sample the
 * inverse). Row-major, uploaded with transpose=GL_TRUE. On-device check:
 * weston-transformed (a direction error = swap the 90/270 pair). */
extern const float awl_gl_xform[8][9];

/* ---- AHardwareBuffer → texture ----
 * EGL_ANDROID_image_native_buffer: the sanctioned path from an AHB (forged
 * over a client dma-buf, or platform-allocated) to a GL_TEXTURE_2D. The
 * texture samples the buffer's true channel order (HAL BGRA_8888 for our
 * forged AR24/XR24 frames) — no shader swizzle. Linear filter, clamp. */
struct awl_gl_tex {
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
};
bool awl_gl_tex_import(AHardwareBuffer* ahb, awl_gl_tex* out);
void awl_gl_tex_release(awl_gl_tex* t);
/* awl_ahb_cache glue: a slot's payload is a heap awl_gl_tex built on first
 * use. Pass awl_gl_tex_payload_destroy as the cache's payload_destroy (must
 * run on the thread that owns the context — the caller's contract). */
void awl_gl_tex_payload_destroy(void* payload);
GLuint awl_gl_slot_texture(struct awl_ahb_slot* s);   /* 0 = import failed (slot untouched) */

/* ---- wl_shm → texture ----
 * The backend-side half of awl_surface_shm_begin/end: a plain GL texture
 * holding the surface's shm content, (re)created on size/format change with
 * a full upload, otherwise refreshed by the frame's damage rect only
 * (glTexSubImage2D straight from the client's pool: one CPU→GPU copy, no
 * intermediate buffer). BGRA memory order → GL_BGRA_EXT upload
 * (GL_EXT_texture_format_BGRA8888; RGBA + texture swizzle when absent).
 * `serial` = the frame serial the texture reflects (pass to begin). */
struct awl_gl_shm_tex {
    GLuint texture = 0;
    uint32_t w = 0, h = 0, format = 0;
    uint64_t serial = 0;
};
struct awl_shm_frame;
bool awl_gl_shm_update(awl_gl_shm_tex* t, const struct awl_shm_frame* f);
void awl_gl_shm_release(awl_gl_shm_tex* t);

/* ---- fences ----
 * fence_fd: native fence (sync_file) covering everything submitted so far on
 * the current context — the release fence for the client buffers a composite
 * sampled / the acquire fence of a buffer it rendered. Flushes. −1 =
 * EGL_ANDROID_native_fence_sync unavailable (caller falls back to glFinish).
 * wait_fence_fd: make the GPU wait for a sync_file before the following
 * commands (EGL_KHR_wait_sync; CPU wait fallback). Takes ownership of fd. */
int  awl_gl_fence_fd(void);
void awl_gl_wait_fence_fd(int fd);

#endif /* AWL_GL_HPP */
