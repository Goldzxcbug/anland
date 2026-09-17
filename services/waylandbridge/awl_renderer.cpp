/* awl_renderer.cpp — per-window GPU rendering (v3 multi-layer, no CPU
 * per-pixel compositing) + dmabuf→AHB wrap machinery
 *
 * Frame source = each layer's buffer queue (awl_bufferqueue.h, logic layer):
 * dma-buf fd + acquire fence + geometry, whatever the client committed
 * (zwp_linux_dmabuf) or the logic layer converted (wl_shm → dma-buf in
 * awl_shmblit.c). Per layer and frame: lock → drain (superseded frames go
 * back to the client) → gethead (waits the acquire fence, takes a reference)
 * → unlock → import (per-layer cache keyed by dma-buf inode: a client
 * cycling its swapchain never re-imports) → draw → put(native fence of this
 * composite) — the element goes back to the client when the ring has
 * dropped it too, with the fence as its release fence.
 *   dmabuf : forged AHardwareBuffer → EGLImage → zero-copy texture
 * Composite = multi-layer quads (root + wl_subsurface child layers, render
 * stack order bottom→top, then the client's wl_pointer.set_cursor image on
 * top) sampled into dst rect → eglSwapBuffers → BufferQueue/SurfaceFlinger
 * present.
 * Per-layer texture state cached by surface id (wl_tex); reclaimed when the
 * layer disappears.
 * wl_surface.set_buffer_transform is applied per layer via the u_xform
 * sample matrix.
 * blend = premultiplied alpha (ONE, ONE_MINUS_SRC_ALPHA); first layer (root)
 * blend off.
 *
 * All rendering happens on the window's dedicated render thread
 * (render_thread_loop).
 */
#include "awl_renderer.hpp"
#include "awl_ahb.hpp"
#include "awl_geom.h"
#include "awl_bufferqueue.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>   /* GL_BGRA_EXT etc. (gl3.h must come first for base types) */
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window.h>
#include <math.h>                 /* round: pixel-grid snap of the root dst */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>               /* close (release fence) */
#include <errno.h>

#include <map>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <vector>

#define AWL_TAG "anland-rd"
#include "awl_log.h"   /* LOGI/LOGE/LOGD (LOGD compiled out unless AWL_LOG_DEBUG) */

/* ---------------- dmabuf → registered AHardwareBuffer ----------------
 * The forging + the per-layer slot cache live in awl_ahb.cpp (shared with
 * the SurfaceControl compositor); the GL renderer hangs its EGLImage+texture
 * on each cache slot's payload. */

static const char* k_vert_src =
    "#version 300 es\n"
    "in vec2 pos;\n"
    "uniform vec2 u_view;   /* window px */\n"
    "uniform vec4 u_dst;    /* dst rect px: x,y=top-left w,h=size (Y down) */\n"
    "uniform vec4 u_uv;     /* sample region transform: uv = u_uv.xy + uv*u_uv.zw (#31 source) */\n"
    "uniform mat3 u_xform;  /* display uv q -> buffer uv (wl_surface.set_buffer_transform) */\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "  vec2 px = vec2(u_dst.x + (pos.x * 0.5 + 0.5) * u_dst.z,\n"
    "                 u_dst.y + (0.5 - pos.y * 0.5) * u_dst.w);\n"
    "  gl_Position = vec4(px.x / u_view.x * 2.0 - 1.0,\n"
    "                     1.0 - px.y / u_view.y * 2.0, 0.0, 1.0);\n"  /* flip Y */
    "  vec2 q = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);\n"
    "  uv = u_uv.xy + (u_xform * vec3(q, 1.0)).xy * u_uv.zw;\n"
    "}\n";

static const char* k_frag_src =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 uv;\n"
    "out vec4 color;\n"
    "uniform sampler2D tex;\n"
    "void main() {\n"
    "  color = texture(tex, uv);\n"   /* channel order comes from the texture format (dmabuf = BGRA_8888) */
    "}\n";

/* wl buffer transform (wl_output.transform 0..7) → sample-uv affine for a
 * layer quad: rows u = a·qx + b·qy + c, v = d·qx + e·qy + f (q = top-down
 * display uv; the buffer holds the content rotated by T, we sample the
 * inverse). Uploaded row-major with transpose=GL_TRUE. On-device check:
 * weston-transformed (a direction error = swap the 90/270 pair). */
static const float k_root_xform[8][9] = {
    /* 0 normal      */ { 1, 0, 0,   0, 1, 0,   0, 0, 1 },
    /* 1 90          */ { 0, 1, 0,  -1, 0, 1,   0, 0, 1 },
    /* 2 180         */ { -1, 0, 1,  0, -1, 1,  0, 0, 1 },
    /* 3 270         */ { 0, -1, 1,  1, 0, 0,   0, 0, 1 },
    /* 4 flipped     */ { -1, 0, 1,  0, 1, 0,   0, 0, 1 },
    /* 5 flipped_90  */ { 0, 1, 0,   1, 0, 0,   0, 0, 1 },
    /* 6 flipped_180 */ { 1, 0, 0,   0, -1, 1,  0, 0, 1 },
    /* 7 flipped_270 */ { 0, -1, 1, -1, 0, 1,   0, 0, 1 },
};

/* Per-layer import cache = awl_ahb_cache (slot identity: dma-buf inode, kernel
 * monotonically allocated, never recycled). Each slot's payload = the GL
 * objects bound to that forged AHB (EGLImage importing it + texture bound to
 * the image). A client cycling N buffers hits the cache from the second lap
 * on: zero gralloc imports, zero EGLImage creation per frame — the per-frame
 * cost is one texture bind. Render-thread only per window. */
struct gl_slot {
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
};

/* Per-layer import state: the shared cache + the entry this frame draws. */
struct wl_tex {
    struct awl_ahb_cache* cache = nullptr;
    GLuint texture = 0;            /* selected for this frame (payload's) */
};

struct wl_window {
    uint64_t id;
    ANativeWindow* nw;        /* self-held reference */
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    GLuint program = 0;
    GLuint vbo = 0;
    /* attribute/uniform locations — fixed at link time, resolved once in
     * window_setup_gl (glGet*ALocation is a driver string lookup per call) */
    GLint a_pos = -1, u_view = -1, u_dst = -1, u_uv = -1, u_xform = -1,
          u_tex = -1;
    int cur_vw = 0, cur_vh = 0;    /* ANativeWindow size cache — valid until
                                    * the logic layer reports a resize
                                    * (awl_window_resize → size_dirty;
                                    * small-window ↔ fullscreen resizes the
                                    * surface in place, no SURFACE re-attach) */
    std::atomic<bool> size_dirty{true};   /* render_frame re-queries once */
    bool logged_frame = false;     /* first-frame log (diagnostics) */
    uint64_t frame_no = 0;         /* LRU clock of the layer import caches */
    std::map<uint64_t, wl_tex> layers;   /* layer id → import cache (includes root's own id) */

    /* dedicated render thread: context bound 1:1 to the thread, requests coalesced via condvar */
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    bool stop = false;
    bool render_req = false;
};

static struct {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = 0;
    EGLContext share_ctx = EGL_NO_CONTEXT;   /* precompiled shared program resources */
    bool inited = false;
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC eglGetNativeClientBuffer;
    /* EGL_ANDROID_native_fence_sync: per-frame release fence (sync_file) for
     * the sampled client buffers; optional — without it releases are
     * immediate and reuse relies on the driver's implicit dma-buf fences */
    PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR = nullptr;
    PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR = nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID = nullptr;
    bool native_fence = false;
} g;

static std::mutex g_map_lock;
static std::map<uint64_t, wl_window*> g_windows;

static void destroy_dmabuf_texture(wl_tex* t);   /* forward reference for window_teardown_gl */

/* ---------------- EGL global init ---------------- */

static bool egl_init(void) {
    if (g.inited) return true;
    /* Double-checked mutex: SURFACE can arrive concurrently from multiple binder
     * threads; failure leaves the flag unset — retried on the next attach (no
     * call_once: a failure there would lock up rendering forever) */
    static std::mutex init_lock;
    std::lock_guard<std::mutex> lk(init_lock);
    if (g.inited) return true;
    g.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g.display == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }
    if (!eglInitialize(g.display, NULL, NULL)) {
        LOGE("eglInitialize: 0x%x", eglGetError());
        return false;
    }
    const char* exts = eglQueryString(g.display, EGL_EXTENSIONS);
    LOGI("EGL extensions: %s", exts ? exts : "none");
    bool has_dmabuf = exts && strstr(exts, "EGL_EXT_image_dma_buf_import");
    bool has_img2d = exts && strstr(exts, "EGL_KHR_gl_texture_2D_image");
    if (!has_dmabuf || !has_img2d)
        LOGE("!! dmabuf import %s / image2d %s",
             has_dmabuf ? "ok" : "MISSING", has_img2d ? "ok" : "MISSING");

    g.eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    g.eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");
    g.glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    g.eglGetNativeClientBuffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)
        eglGetProcAddress("eglGetNativeClientBufferANDROID");
    if (!g.eglCreateImageKHR || !g.eglDestroyImageKHR ||
        !g.glEGLImageTargetTexture2DOES || !g.eglGetNativeClientBuffer) {
        LOGE("EGLImage procs missing");
        return false;
    }
    if (exts && strstr(exts, "EGL_ANDROID_native_fence_sync")) {
        g.eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
        g.eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
        g.eglDupNativeFenceFDANDROID = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)
            eglGetProcAddress("eglDupNativeFenceFDANDROID");
        g.native_fence = g.eglCreateSyncKHR && g.eglDestroySyncKHR && g.eglDupNativeFenceFDANDROID;
    }
    LOGI("EGL native fence sync (release fences): %s", g.native_fence ? "yes" : "NO — immediate releases");

    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0,
        EGL_NONE,
    };
    EGLint n = 0;
    if (!eglChooseConfig(g.display, cfg_attr, &g.config, 1, &n) || n < 1) {
        LOGE("eglChooseConfig: 0x%x n=%d", eglGetError(), n);
        return false;
    }
    g.inited = true;
    return true;
}

static GLuint compile(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        LOGE("shader: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static bool window_setup_gl(wl_window* w) {
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    w->context = eglCreateContext(g.display, g.config, EGL_NO_CONTEXT, ctx_attr);
    if (w->context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext: 0x%x", eglGetError());
        return false;
    }
    w->surface = eglCreateWindowSurface(g.display, g.config,
                                        (EGLNativeWindowType)w->nw, NULL);
    if (w->surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface: 0x%x", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(g.display, w->surface, w->surface, w->context)) {
        LOGE("eglMakeCurrent: 0x%x", eglGetError());
        return false;
    }

    GLuint vs = compile(GL_VERTEX_SHADER, k_vert_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, k_frag_src);
    if (!vs || !fs) return false;
    w->program = glCreateProgram();
    glAttachShader(w->program, vs);
    glAttachShader(w->program, fs);
    glLinkProgram(w->program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(w->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        LOGE("program link failed");
        return false;
    }
    w->a_pos = glGetAttribLocation(w->program, "pos");
    w->u_view = glGetUniformLocation(w->program, "u_view");
    w->u_dst = glGetUniformLocation(w->program, "u_dst");
    w->u_uv = glGetUniformLocation(w->program, "u_uv");
    w->u_xform = glGetUniformLocation(w->program, "u_xform");
    w->u_tex = glGetUniformLocation(w->program, "tex");

    static const float quad[] = {
        -1, -1,  1, -1,  -1, 1,
        -1,  1,  1, -1,   1, 1,
    };
    glGenBuffers(1, &w->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, w->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    /* layer textures created on demand (render_thread_loop, see wl_tex) */
    glViewport(0, 0, ANativeWindow_getWidth(w->nw), ANativeWindow_getHeight(w->nw));
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    /* setup runs on the calling thread (binder pool), rendering on the window's dedicated thread — release current */
    eglMakeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    LOGI("window %llu GL ready %dx%d", (unsigned long long)w->id,
         ANativeWindow_getWidth(w->nw), ANativeWindow_getHeight(w->nw));
    {
        const char* gle = (const char*)glGetString(GL_EXTENSIONS);
        LOGI("GL ext has memory_object_fd: %d",
             gle && strstr(gle, "GL_EXT_memory_object_fd") ? 1 : 0);
        const char* glv = (const char*)glGetString(GL_VERSION);
        LOGI("GL version: %s", glv ? glv : "?");
    }
    return true;
}

static void window_teardown_gl(wl_window* w) {
    if (w->context != EGL_NO_CONTEXT) {
        eglMakeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (w->program) glDeleteProgram(w->program);
        if (w->vbo) glDeleteBuffers(1, &w->vbo);
        for (auto& kv : w->layers) destroy_dmabuf_texture(&kv.second);
        w->layers.clear();
        if (w->surface != EGL_NO_SURFACE) eglDestroySurface(g.display, w->surface);
        eglDestroyContext(g.display, w->context);
        w->context = EGL_NO_CONTEXT;
        w->surface = EGL_NO_SURFACE;
        w->program = w->vbo = 0;
    }
}

/* ---------------- public API ---------------- */

static void render_thread_loop(wl_window* w);   /* defined at end of file */
void awl_renderer_request_render(uint64_t id);  /* awl_renderer.hpp; used by render_frame's re-arm */

/* Detach and reclaim a window entry (map removal inside g_map_lock, join/free
 * entirely outside the lock — join must not hold g_map_lock: it would stall
 * every window's request_render, which runs on the client dispatch thread).
 * Returns whether an entry was actually reclaimed. */
static bool renderer_teardown_locked_out(uint64_t id) {
    wl_window* w = NULL;
    {
        std::lock_guard<std::mutex> lk(g_map_lock);
        auto it = g_windows.find(id);
        if (it == g_windows.end()) return false;
        w = it->second;
        g_windows.erase(it);
    }
    {
        std::lock_guard<std::mutex> lk(w->m);
        w->stop = true;
        w->cv.notify_all();
    }
    if (w->th.joinable()) w->th.join();
    window_teardown_gl(w);
    ANativeWindow_release(w->nw);
    delete w;
    return true;
}

int awl_renderer_attach(uint64_t id, ANativeWindow* nw) {
    /* attach/detach for the same id fully serialized: prevents a g_windows[id]
     * overwrite leak when concurrent SURFACE (re-attach) and detach interleave
     * (old thread never joined). Lifecycle-level operation, low frequency —
     * serialization is harmless; request_render bypasses this lock. */
    static std::mutex attach_lock;
    std::lock_guard<std::mutex> alkg(attach_lock);

    /* Tear down any existing entry with the same id first (re-attach = old
     * render target necessarily stale/replaced): same semantics as the
     * detach branch below */
    renderer_teardown_locked_out(id);

    if (nw) {
        if (!egl_init()) return -1;
        wl_window* w = new wl_window();
        w->id = id;
        ANativeWindow_acquire(nw);
        w->nw = nw;
        if (!window_setup_gl(w)) {      /* GL setup on the calling thread (binder pool, concurrent) */
            ANativeWindow_release(nw);
            delete w;
            return -1;
        }
        w->cur_vw = ANativeWindow_getWidth(nw);
        w->cur_vh = ANativeWindow_getHeight(nw);
        w->th = std::thread(render_thread_loop, w);   /* dedicated render thread */
        std::lock_guard<std::mutex> lk(g_map_lock);
        g_windows[id] = w;
        return 0;
    }
    return 0;
}


/* ---------------- per-layer import cache ---------------- */

/* GL payload on an awl_ahb slot (invoked by the cache on eviction/teardown —
 * the calling render thread holds the current EGL context) */
static void gl_payload_destroy(void* p) {
    gl_slot* s = (gl_slot*)p;
    if (s->texture) glDeleteTextures(1, &s->texture);
    if (s->image != EGL_NO_IMAGE_KHR) g.eglDestroyImageKHR(g.display, s->image);
    delete s;
}

static void destroy_dmabuf_texture(wl_tex* t) {
    if (t->cache) {
        awl_ahb_cache_destroy(t->cache);   /* releases the forged AHBs */
        t->cache = nullptr;
    }
    t->texture = 0;
}

static void tex_params_default(void) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* Resolve the GL objects for this frame's dma-buf: shared cache (awl_ahb.cpp)
 * hit → the payload's texture binds; miss → the cache forged a fresh AHB and
 * the EGLImage + texture are built on it here. On success t->texture is the
 * texture to bind. frame = the window's frame counter (LRU clock). */
static bool import_dmabuf_texture(wl_tex* t, const struct awl_bq_buffer* b, uint64_t frame) {
    /* (The write-fence gate that used to sit here — the client's paint may
     * still be in flight at commit, resize-ack frames read as zeros for a
     * few ms — is the queue's job: gethead returned this element only after
     * its acquire fence signaled, explicit or exported from the dma-buf's
     * own write fences.) */
    if (!t->cache) t->cache = awl_ahb_cache_create(gl_payload_destroy);
    if (!t->cache) return false;
    struct awl_ahb_slot* s = awl_ahb_cache_get(t->cache, b,
                                               AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
                                               frame);
    if (!s) return false;   /* forge refused — cache untouched, retry next frame */
    gl_slot* gl = (gl_slot*)s->payload;
    if (!gl) {   /* fresh forge: EGLImage + texture on it */
        gl = new gl_slot();
        EGLClientBuffer cb = g.eglGetNativeClientBuffer(s->ahb);   /* AHB → EGLClientBuffer (the sanctioned path) */
        if (!cb) {
            LOGE("eglGetNativeClientBuffer == NULL");
            delete gl;
            return false;
        }
        gl->image = g.eglCreateImageKHR(g.display, EGL_NO_CONTEXT,
                                        EGL_NATIVE_BUFFER_ANDROID, cb, NULL);
        if (gl->image == EGL_NO_IMAGE_KHR) {
            LOGE("eglCreateImageKHR(native buffer): 0x%x (%ux%u stride=%u)",
                 eglGetError(), b->width, b->height, b->stride);
            delete gl;
            return false;
        }
        glGenTextures(1, &gl->texture);
        glBindTexture(GL_TEXTURE_2D, gl->texture);
        tex_params_default();
        g.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, gl->image);
        GLenum terr = glGetError();
        if (terr != GL_NO_ERROR) LOGE("EGLImageTargetTexture: 0x%x", terr);
        s->payload = gl;
        LOGD("AHB import ok %ux%u stride=%u ino=%llu",
             b->width, b->height, b->stride, (unsigned long long)s->ino);
    }
    t->texture = gl->texture;
    return true;
}

/* (The shm glTexSubImage2D upload path is gone: wl_shm content reaches the
 * renderer as a dma-buf the logic layer converted — awl_shmblit.c — and is
 * imported like any other.) */

/* Native fence of everything submitted so far on this context (sync_file
 * fd, caller owns; -1 = unavailable). eglDupNativeFenceFDANDROID flushes. */
static int frame_release_fence(void) {
    if (!g.native_fence) return -1;
    EGLSyncKHR sync = g.eglCreateSyncKHR(g.display, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (sync == EGL_NO_SYNC_KHR) return -1;
    int fd = g.eglDupNativeFenceFDANDROID(g.display, sync);
    g.eglDestroySyncKHR(g.display, sync);
    return fd >= 0 ? fd : -1;
}

/* ---------------- per-window render thread ----------------
 * context stays resident on this thread (MakeCurrent once); requests
 * coalesced/deduped; glFinish/swap block only this window — multi-window
 * parallelism */

static void render_frame(wl_window* w) {
    /* Layer snapshot (root first, child layers in render stack order bottom→top);
     * layers that fail to fetch / have no buffer are skipped.
     * #31 zoom: coordinates/sizes are root logical pixels; dst = (logical −
     * geometry origin) × s + o with the root's view transform (1:1 at Z for
     * content following the configure, scale_mode placement otherwise),
     * snapped to the pixel grid (kwin snapToPixelGrid). The geometry-origin
     * alignment (chrome buffer carries 16/10px shadow margins) is shared
     * with the input mapping. */
    awl_layer_info_t lay[AWL_MAX_LAYERS + 1];   /* +1: client cursor image appended on top */
    int n = awl_surface_get_layers(w->id, lay, AWL_MAX_LAYERS);
    if (n <= 0) return;
    /* wl_pointer.set_cursor image of the pointer-focused client: composited
     * above every layer of this window (x,y = pointer − hotspot in the same
     * root logical coordinates as the stack; never part of hit-testing).
     * Drawn/presented like any layer → the cursor surface gets frame_done
     * (animated cursors) and deferred buffer release. */
    if (awl_pointer_cursor_layer(w->id, &lay[n])) n++;
    /* Root view transform (logic layer, the same snapshot the input inverse
     * uses): geometry origin + logical→view scale/offset. A root whose
     * content has the configured size maps 1:1 at Z with no offset; only
     * content that ignores the configure gets the scale_mode placement (its
     * letterbox bars stay the clear color below). Degenerate → identity. */
    awl_view_xform_t xf;
    awl_surface_get_view_xform(w->id, &xf);

    /* size cache — dropped when the logic layer reports a resize
     * (awl_window_resize → awl_renderer_window_resized); re-query renders the
     * frame at the fresh viewport/u_view = the original full-screen flush */
    if (w->size_dirty.load(std::memory_order_relaxed) || w->cur_vw <= 0 ||
        w->cur_vh <= 0) {
        w->cur_vw = ANativeWindow_getWidth(w->nw);
        w->cur_vh = ANativeWindow_getHeight(w->nw);
        w->size_dirty.store(false, std::memory_order_relaxed);
    }
    int vw = w->cur_vw;
    int vh = w->cur_vh;
    LOGD("win %llu render: view=%dx%d n=%d root=%llu xf(s=%.3f,%.3f o=%.1f,%.1f go=%d,%d)",
         (unsigned long long)w->id, vw, vh, n,
         (unsigned long long)lay[0].surface_id,
         xf.sx, xf.sy, xf.ox, xf.oy, xf.gox, xf.goy);

    glViewport(0, 0, vw, vh);
    glClearColor(0, 0, 0, 1);   /* letterbox bars for fit/center modes */
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(w->program);
    glBindBuffer(GL_ARRAY_BUFFER, w->vbo);
    glEnableVertexAttribArray((GLuint)w->a_pos);
    glVertexAttribPointer((GLuint)w->a_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glUniform2f(w->u_view, (float)vw, (float)vh);
    /* dmabuf textures are imported as their true channel order (HAL
     * BGRA_8888, see import_dmabuf_texture) and shm uploads convert on
     * GL_BGRA_EXT upload — both sample correct as-is, no swizzle. The
     * transform matrix maps the display quad uv through the wl buffer
     * transform (set_buffer_transform, whole-buffer inverse rotation). */
    glUniform1i(w->u_tex, 0);
    glActiveTexture(GL_TEXTURE0);

    /* Per-layer frame source: resolve the queue (own reference — a layer
     * dying mid-frame cannot free it), lock → drain superseded frames back
     * to the client → take a reference on the newest complete head (waits
     * its acquire fence, bounded) → arm the fence waiter for a still-
     * incomplete newer frame → unlock. Microseconds under the lock: the
     * element reference, not the lock, keeps the frame alive and its memory
     * open while we import and draw; a commit-time drain on the dispatch
     * thread almost never finds the lock busy. NULL / NULL-marker = nothing
     * to draw for this layer. */
    struct awl_bq_buffer* head[AWL_MAX_LAYERS + 1];
    bool more = false;   /* a queue still holds a newer, not-yet-complete frame */
    for (int i = 0; i < n; i++) {
        head[i] = NULL;
        struct awl_bufferqueue* q = awl_surface_queue_ref(lay[i].surface_id);
        if (!q) continue;
        awl_bufferqueue_lock(q);
        awl_bufferqueue_drain(q);
        head[i] = awl_bufferqueue_gethead(q, 100);
        if (awl_bufferqueue_pending(q) > 0) more = true;
        awl_bufferqueue_arm(q);   /* pending incomplete frame: its fence, not the next vsync, releases the head */
        awl_bufferqueue_unlock(q);
        awl_bufferqueue_unref(q);
    }
    w->frame_no++;

    uint64_t seen[AWL_MAX_LAYERS + 1];
    int nseen = 0;
    bool drew = false;
    for (int i = 0; i < n; i++) {
        const struct awl_bq_buffer* b = head[i];
        if (!b || b->dmabuf_fd < 0) continue;   /* no frame yet / detached (NULL marker) */
        wl_tex& t = w->layers[lay[i].surface_id];   /* layers seen this frame */
        seen[nseen++] = lay[i].surface_id;
        bool ok = import_dmabuf_texture(&t, b, w->frame_no);
        if (!ok) continue;

        /* first layer (root) writes directly with blend off; child layers stack on top with premultiplied alpha */
        if (i == 0 || !drew) glDisable(GL_BLEND);
        else {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        /* Pixel-grid snap (awl_snap_extent, awl_renderer.hpp): integer origin,
         * size = the buffer's own pixel count when it is the Z-scaled rendition
         * of the logical size — GL_LINEAR then samples texel centers: lossless.
         * A fractional origin/size would resample the whole buffer (half-pixel
         * blur) even at scale 1. */
        double rsw, rsh;
        awl_layer_sampled(&lay[i], b->width, b->height, &rsw, &rsh);
        glUniform4f(w->u_dst,
                    (float)round(((double)lay[i].x - (double)xf.gox) * xf.sx + xf.ox),
                    (float)round(((double)lay[i].y - (double)xf.goy) * xf.sy + xf.oy),
                    (float)awl_snap_extent(lay[i].w, xf.sx, rsw),
                    (float)awl_snap_extent(lay[i].h, xf.sy, rsh));
        glUniform4f(w->u_uv, lay[i].u0, lay[i].v0, lay[i].su, lay[i].sv);
        glUniformMatrix3fv(w->u_xform, 1, GL_TRUE,
                           k_root_xform[lay[i].transform & 7]);
        glBindTexture(GL_TEXTURE_2D, t.texture);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        drew = true;

        if (!w->logged_frame) {
            w->logged_frame = true;
            LOGI("window %llu frame: layers=%d [%d]=%llu %ux%u stride=%u "
                 "fmt=%c%c%c%c mod=0x%llx xform=%d (glerr=0x%x)",
                 (unsigned long long)w->id, n, i,
                 (unsigned long long)lay[i].surface_id,
                 b->width, b->height, b->stride,
                 (char)(b->format & 0xff), (char)((b->format >> 8) & 0xff),
                 (char)((b->format >> 16) & 0xff), (char)((b->format >> 24) & 0xff),
                 (unsigned long long)b->modifier,
                 lay[i].transform, glGetError());
        }
    }
    if (!drew) {   /* not even root has a usable buffer — don't spin on a swap */
        for (int i = 0; i < n; i++) awl_bufferqueue_put(head[i], -1);   /* nothing sampled */
        if (more) awl_renderer_request_render(w->id);
        return;
    }

    /* vanished layers (bubble hidden/destroyed): reclaim texture and AHB references */
    for (auto it = w->layers.begin(); it != w->layers.end();) {
        bool found = false;
        for (int k = 0; k < nseen && !found; k++)
            found = (seen[k] == it->first);
        if (!found) {
            destroy_dmabuf_texture(&it->second);
            it = w->layers.erase(it);
        } else {
            ++it;
        }
    }

    /* Release fence for the client buffers this composite sampled: a native
     * fence inserted after the draws (signals when the GPU is done reading;
     * the dup flushes the command stream). put() merges it into every held
     * element and drops our reference — when the ring has already popped
     * the element (a newer frame superseded it while we drew) it goes back
     * to the client right here, with the fence: explicit-sync clients get it
     * as the fenced_release, implicit-sync clients find it as a read fence
     * in the dma-buf reservation (bq_release_cb). Without native fences the
     * only remaining guarantee is glFinish (GPU idle = reads done). Nothing
     * of this waits for the vsync-blocking swap below: the client's buffer
     * turnaround is bounded by the GPU, not by the display. No glFinish
     * otherwise: eglSwapBuffers submits with its own native fence (SF waits
     * GPU-side before scanout), frame_done goes out at submit time so the
     * client's next frame overlaps this one's GPU composite. */
    glDisable(GL_BLEND);
    int rel = frame_release_fence();
    if (rel < 0) glFinish();
    for (int i = 0; i < n; i++) awl_bufferqueue_put(head[i], rel);
    if (rel >= 0) close(rel);
    /* a newer frame arrived while we drew but was not complete yet: present
     * it as soon as the swap returns (the waiter hands buffers back, this
     * re-arm keeps the screen current) */
    if (more) awl_renderer_request_render(w->id);
    if (!eglSwapBuffers(g.display, w->surface)) {
        LOGE("eglSwapBuffers: 0x%x", eglGetError());
        return;
    }
#ifdef AWL_LOG_DEBUG   /* two driver queries per swap — debug builds only (LOGD args alone would not evaluate them) */
    {
        EGLint sw = 0, sh = 0;
        eglQuerySurface(g.display, w->surface, EGL_WIDTH, &sw);
        eglQuerySurface(g.display, w->surface, EGL_HEIGHT, &sh);
        LOGD("win %llu swapped: egl surface %dx%d (view %dx%d)",
             (unsigned long long)w->id, sw, sh, vw, vh);
    }
#endif

    /* frame_done for each layer (child layers piggyback on the parent window's presentation) */
    for (int k = 0; k < nseen; k++)
        awl_surface_presented(seen[k]);
}

static void render_thread_loop(wl_window* w) {
    if (!eglMakeCurrent(g.display, w->surface, w->surface, w->context)) {
        LOGE("window %llu render thread MakeCurrent: 0x%x",
             (unsigned long long)w->id, eglGetError());
        std::lock_guard<std::mutex> lk(w->m);
        w->stop = true;
        return;
    }
    /* vsync throttling: high-frequency commits from desync child layers (bubble) must not become unthrottled frame swaps */
    eglSwapInterval(g.display, 1);
    for (;;) {
        std::unique_lock<std::mutex> lk(w->m);
        w->cv.wait(lk, [&] { return w->stop || w->render_req; });
        bool stopping = w->stop;
        w->render_req = false;
        lk.unlock();
        if (stopping) break;
        render_frame(w);
    }
    eglMakeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void awl_renderer_request_render(uint64_t id) {
    /* access w under the map lock: mutually exclusive with detach's removal (no use-after-free) */
    std::lock_guard<std::mutex> lk(g_map_lock);
    auto it = g_windows.find(id);
    if (it == g_windows.end()) return;   /* Activity not ready yet; request re-issued after attach */
    wl_window* w = it->second;
    std::lock_guard<std::mutex> lw(w->m);
    w->render_req = true;
    w->cv.notify_all();
}

/* Logic layer (awl_window_resize, any thread): the Android window resized in
 * place — the cached ANativeWindow size is stale. Drop it and wake the render
 * thread: the next frame re-queries and presents at the new size (the
 * original full-screen resize path). Coalesced like a render request. */
void awl_renderer_window_resized(uint64_t id) {
    std::lock_guard<std::mutex> lk(g_map_lock);
    auto it = g_windows.find(id);
    if (it == g_windows.end()) return;
    wl_window* w = it->second;
    w->size_dirty.store(true, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lw(w->m);
    w->render_req = true;
    w->cv.notify_all();
}

void awl_renderer_shutdown(void) {
    std::vector<wl_window*> wins;
    {
        std::lock_guard<std::mutex> lk(g_map_lock);
        for (auto& kv : g_windows) wins.push_back(kv.second);
        g_windows.clear();
    }
    for (wl_window* w : wins) {
        {   /* same as detach: stop+join, then release */
            std::lock_guard<std::mutex> lk(w->m);
            w->stop = true;
            w->cv.notify_all();
        }
        if (w->th.joinable()) w->th.join();
        window_teardown_gl(w);
        ANativeWindow_release(w->nw);
        delete w;
    }
    if (g.display != EGL_NO_DISPLAY) {
        eglTerminate(g.display);
        g.display = EGL_NO_DISPLAY;
    }
    g.inited = false;
}
