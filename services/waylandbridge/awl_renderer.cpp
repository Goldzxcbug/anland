/* awl_renderer.cpp — per-window GPU rendering (v3 multi-layer, no CPU
 * per-pixel compositing) + dmabuf→AHB wrap machinery
 *
 * Frame source = each layer's buffer queue (awl_bufferqueue.h, logic layer):
 * dma-buf fd + acquire fence + geometry, whatever the client committed
 * (zwp_linux_dmabuf) or the logic layer converted (wl_shm → dma-buf in
 * awl_shmblit.c). Per layer and frame: lock → drain (superseded frames go
 * back to the client) → gethead (waits the acquire fence) → import → draw;
 * after the composite is submitted a native fence of this frame is attached
 * to every sampled head (set_release_fence → the client's release fence /
 * implicit-sync read fence) and the queues are unlocked.
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
#include <unistd.h>               /* dup / getpid */
#include <sys/socket.h>           /* socketpair / sendmsg / SCM_RIGHTS */
#include <sys/mman.h>             /* donor blob patching */
#include <sys/stat.h>             /* fstat: dma-buf inode identity */
#include <fcntl.h>
#include <errno.h>

/* Official VNDK API (vndk/hardware_buffer.h); no header in the NDK sysroot,
 * symbol exported by libnativewindow.so (already linked via CMake) */
extern "C" const struct native_handle* AHardwareBuffer_getNativeHandle(
    const AHardwareBuffer* buffer);
struct native_handle { int version; int numFds; int numInts; int data[]; };

#include <map>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <vector>

#define AWL_TAG "anland-rd"
#include "awl_log.h"   /* LOGI/LOGE/LOGD (LOGD compiled out unless AWL_LOG_DEBUG) */

/* ---------------- dmabuf → AHardwareBuffer (AOSP construction logic ported) ---- */

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

/* Per-surface dmabuf slot: ONE AHardwareBuffer per surface, swapped per
 * arriving buffer (snapalloc donor scheme, see wrap_dmabuf_ahb). When the
 * dmabuf changes the AHB is re-forged with a fresh identity (kgsl binds the
 * memory at import — an in-place fd swap would leave the GPU sampling the
 * old dmabuf); the re-forge uses the OLD AHB itself as the donor (its
 * metadata blob already carries this geometry — no allocation), and the old
 * AHB's release closes the swapped-out dmabuf fd. The blob stays alive
 * through the relay: each forged AHB's handle holds its own fd dup. After a
 * swap the consumer MUST re-import: a new EGLImage for the texture.
 * Render-thread only per surface. */
struct awl_ahb_slot {
    AHardwareBuffer* ahb = NULL;   /* wraps the current dmabuf */
    uint64_t ino = 0;              /* dma-buf identity (fstat inode) */
    uint32_t w = 0, h = 0, stride = 0;
};

/* Per-layer texture state. dmabuf: the surface's awl_ahb_slot (one AHB,
 * swapped per arriving buffer) + the EGLImage currently importing it. */
struct wl_tex {
    GLuint texture = 0;
    uint32_t tex_w = 0, tex_h = 0;
    bool tex_is_image = false;     /* external-memory texture (dmabuf) */
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    awl_ahb_slot slot;
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
    std::map<uint64_t, wl_tex> layers;   /* layer id → texture (includes root's own id) */

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
        for (auto& kv : w->layers) {
            destroy_dmabuf_texture(&kv.second);
            if (kv.second.texture) glDeleteTextures(1, &kv.second.texture);
        }
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

/* dmabuf → registered AHardwareBuffer — donor-blob supplies metadata
 * (device-verified scheme, see memory snapalloc-ahb-construction)
 *
 * QCOM snapalloc's importBuffer requires the handle to carry a vendor
 * descriptor:
 *   fd[0] = pixel dmabuf
 *   fd[1] = 28KB metadata blob (geometry ground truth; validateBufferSize
 *   checks STRIDE against it)
 * The container kgsl dmabuf lacks this descriptor → plain GB01+fd rejected
 * (error 2 / rc=5).
 *
 * Verified on device (/tmp/hswap*.c, /tmp/gpuverify.c):
 *   - blob mmap RW is writable, no per-buffer checksum
 *   - Retain does not verify the kernel binding between pixel fd and blob
 *   - a 4x4 mini donor's blob, fully patched, serves any geometry
 *     (stride/height/size/width all forgeable; GPU samples strictly by the
 *     patched stride — checkerboard 182528/182528)
 *
 * All calls go through official abstraction layers: AHardwareBuffer_allocate
 * (NDK) / AHardwareBuffer_getNativeHandle (VNDK) /
 * AHardwareBuffer_recvHandleFromUnixSocket (NDK, libs/ui/GraphicBuffer.cpp
 * flatten wire convention) / EGL_ANDROID_image_native_buffer.
 * Blob offsets are not hardcoded — self-calibrated at startup by diffing
 * two-geometry donors; on failure, report and refuse. */

struct ahb_calib {
    bool ok = false;
    uint32_t calib_fmt = 0;        /* HAL format used for calibration (recalibrated per format) */
    int blob_size = 0;            /* donor fd[1] byte count (measured 28672) */
    int num_ints = 0;             /* handle numInts (measured 34) */
    int stride_px_off[8], n_stride_px = 0;   /* blob offset: stride (pixels) */
    int stride_b_off[8],  n_stride_b = 0;    /* blob offset: stride (bytes) */
    int height_off[8],    n_height = 0;      /* blob offset: height */
    int size_off[8],      n_size = 0;        /* blob offset: allocated size (aligned, = pixel dmabuf size) */
    int size_exact_off[8], n_size_exact = 0; /* blob offset: exact size (stride*h*4) */
    int extent_off[8],    n_extent = 0;      /* blob offset: size+constant */
    long extent_const = 0;
    int idx_stride_px = -1;       /* handle ints index */
    int idx_height[2] = {-1, -1}; /* height appears twice (measured [3]/[5]) */
    int n_idx_height = 0;
    int idx_width = -1;
    int idx_size = -1;
    int idx_stride_b = -1;
};
/* One slot per HAL format (keyed lookup — concurrent windows never evict each
 * other's calibration). Since the BGRA_8888 switch all dmabufs (AR24/XR24) map
 * to a single HAL format, in practice one slot covers everything; the table
 * stays generic for future formats. */
static struct ahb_calib k_calibs[8];
static int k_n_calibs = 0;
static struct ahb_calib* calib_slot(uint32_t fmt) {
    for (int i = 0; i < k_n_calibs; i++)
        if (k_calibs[i].calib_fmt == fmt) return &k_calibs[i];
    if (k_n_calibs >= (int)(sizeof(k_calibs) / sizeof(k_calibs[0])))
        return NULL;
    struct ahb_calib* c = &k_calibs[k_n_calibs++];
    memset(c, 0, sizeof(*c));
    c->calib_fmt = fmt;
    return c;
}

/* Get the donor's blob; returns fd (-1 on failure) */
static int donor_blob_fd(const AHardwareBuffer* ahb) {
    const native_handle* nh = AHardwareBuffer_getNativeHandle(ahb);
    if (!nh || nh->numFds != 2) {
        LOGE("donor handle layout unexpected (numFds=%d) — not a snapalloc layout, refusing",
             nh ? nh->numFds : -1);
        return -1;
    }
    return nh->data[1];
}
static int donor_pixel_fd(const AHardwareBuffer* ahb) {
    const native_handle* nh = AHardwareBuffer_getNativeHandle(ahb);
    return nh ? nh->data[0] : -1;
}

/* Locate blob/ints offsets by diffing two-geometry donors (expected values all
 * taken from the donors' own describe; no stride rule assumed) */
static void calib_collect(const uint32_t* ba, const uint32_t* bb, long sz,
                          uint32_t ea, uint32_t eb,
                          int* offs, int* n, int max) {
    *n = 0;
    for (long o = 0; o + 4 <= sz && *n < max; o += 4)
        if (ba[o/4] == ea && bb[o/4] == eb && ea != eb)
            offs[(*n)++] = (int)o;
}
/* Calibrate the given HAL format (returns its slot; already-calibrated hits return immediately) */
static struct ahb_calib* ahb_calibrate(uint32_t fmt) {
    struct ahb_calib* kc = calib_slot(fmt);
    if (!kc) {
        LOGE("calib slots full (concurrent HAL formats >8) — refusing");
        return NULL;
    }
    if (kc->ok) return kc;

    const uint32_t W[2] = {300, 1134}, H[2] = {300, 567};
    AHardwareBuffer* d[2] = {NULL, NULL};
    uint32_t stride_px[2] = {0, 0}, size[2] = {0, 0};
    uint32_t* map[2] = {NULL, NULL};
    bool mapped[2] = {false, false};
    int bfd[2] = {-1, -1};
    long bsz = 0;
    bool ok = false;

    for (int i = 0; i < 2; i++) {
        AHardwareBuffer_Desc dd = {};
        dd.width = W[i]; dd.height = H[i];
        dd.format = fmt;
        dd.layers = 1; dd.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
        if (AHardwareBuffer_allocate(&dd, &d[i]) != 0) {
            LOGE("calib donor %d allocation failed", i);
            goto out;
        }
        AHardwareBuffer_Desc got;
        AHardwareBuffer_describe(d[i], &got);
        stride_px[i] = got.stride;
        /* blob has two size semantics (probe-measured, e.g. 300x300: alloc
         * 0x5e000 / exact 0x5dc00): alloc form = pixel dmabuf size, exact
         * form = stride*h*4 */
        size[i] = (uint32_t)lseek(donor_pixel_fd(d[i]), 0, SEEK_END);
    }

    for (int i = 0; i < 2; i++) {
        bfd[i] = donor_blob_fd(d[i]);
        if (bfd[i] < 0) goto out;
    }
    bsz = lseek(bfd[0], 0, SEEK_END);
    if (bsz <= 0 || bsz != lseek(bfd[1], 0, SEEK_END)) {
        LOGE("calib: donor blob size abnormal %ld", bsz);
        goto out;
    }
    for (int i = 0; i < 2; i++) {
        map[i] = (uint32_t*)mmap(NULL, bsz, PROT_READ, MAP_SHARED, bfd[i], 0);
        if (map[i] == MAP_FAILED) { LOGE("calib: blob mmap %s", strerror(errno)); goto out; }
        mapped[i] = true;
    }
    {
    const native_handle* nh[2] = {AHardwareBuffer_getNativeHandle(d[0]),
                                  AHardwareBuffer_getNativeHandle(d[1])};
    if (nh[0]->numInts != nh[1]->numInts || nh[0]->numInts > 64) {
        LOGE("calib: numInts mismatch %d/%d", nh[0]->numInts, nh[1]->numInts);
        goto out;
    }
    kc->num_ints = nh[0]->numInts;
    const int* ints[2] = {&nh[0]->data[2], &nh[1]->data[2]};

    /* blob fields (byte offsets) */
    calib_collect(map[0], map[1], bsz, stride_px[0], stride_px[1],
                  kc->stride_px_off, &kc->n_stride_px, 8);
    calib_collect(map[0], map[1], bsz, stride_px[0]*4, stride_px[1]*4,
                  kc->stride_b_off, &kc->n_stride_b, 8);
    calib_collect(map[0], map[1], bsz, H[0], H[1],
                  kc->height_off, &kc->n_height, 8);
    calib_collect(map[0], map[1], bsz, size[0], size[1],
                  kc->size_off, &kc->n_size, 8);
    calib_collect(map[0], map[1], bsz,
                  stride_px[0]*H[0]*4, stride_px[1]*H[1]*4,
                  kc->size_exact_off, &kc->n_size_exact, 8);
    /* extent-form fields: blob[k] - size is the same constant */
    kc->n_extent = 0;
    kc->extent_const = 0;
    for (long o = 0; o + 4 <= bsz && kc->n_extent < 8; o += 4) {
        long da = (long)map[0][o/4] - size[0], db = (long)map[1][o/4] - size[1];
        if (da == db && da > 0 && da < 0x100000) {
            kc->extent_off[kc->n_extent++] = (int)o;
            kc->extent_const = da;
        }
    }

    /* handle ints indices */
    kc->idx_stride_px = -1;
    kc->n_idx_height = 0;
    kc->idx_width = kc->idx_size = kc->idx_stride_b = -1;
    for (int k = 0; k < kc->num_ints; k++) {
        if (ints[0][k] == (int)stride_px[0] && ints[1][k] == (int)stride_px[1])
            kc->idx_stride_px = k;
        if (ints[0][k] == (int)H[0] && ints[1][k] == (int)H[1]) {
            if (kc->n_idx_height < 2) kc->idx_height[kc->n_idx_height++] = k;
        }
        if (ints[0][k] == (int)W[0] && ints[1][k] == (int)W[1])
            kc->idx_width = k;
        if (ints[0][k] == (int)size[0] && ints[1][k] == (int)size[1])
            kc->idx_size = k;
        if (ints[0][k] == (int)(stride_px[0]*4) && ints[1][k] == (int)(stride_px[1]*4))
            kc->idx_stride_b = k;
    }

    if (!kc->n_stride_px || !kc->n_stride_b || !kc->n_height ||
        !kc->n_size || !kc->n_size_exact || !kc->n_extent ||
        kc->idx_stride_px < 0 || !kc->n_idx_height ||
        kc->idx_width < 0 || kc->idx_size < 0 || kc->idx_stride_b < 0) {
        LOGE("calib failed: stride_px=%d stride_b=%d height=%d size=%d/%d extent=%d "
             "idx(spx=%d h=%d w=%d size=%d sb=%d) — vendor layout changed, refusing to forge",
             kc->n_stride_px, kc->n_stride_b, kc->n_height,
             kc->n_size, kc->n_size_exact, kc->n_extent,
             kc->idx_stride_px, kc->n_idx_height, kc->idx_width,
             kc->idx_size, kc->idx_stride_b);
        goto out;
    }
    kc->blob_size = (int)bsz;
    kc->calib_fmt = fmt;
    kc->ok = true;
    ok = true;
    LOGI("snapalloc calib ok (fmt=%u): blob=%dB ints=%d "
         "stride_px@%d,%d stride_b@%d,%d h@%d,%d size@%d,%d ext+0x%lx "
         "idx(spx=%d h=[%d,%d] w=%d size=%d sb=%d)",
         fmt, kc->blob_size, kc->num_ints,
         kc->stride_px_off[0], kc->stride_px_off[1],
         kc->stride_b_off[0], kc->stride_b_off[1],
         kc->height_off[0], kc->height_off[1],
         kc->size_off[0], kc->size_off[1], kc->extent_const,
         kc->idx_stride_px, kc->idx_height[0],
         kc->n_idx_height > 1 ? kc->idx_height[1] : -1,
         kc->idx_width, kc->idx_size, kc->idx_stride_b);
    }
out:
    for (int i = 0; i < 2; i++) {
        if (mapped[i]) munmap(map[i], bsz);
        if (d[i]) AHardwareBuffer_release(d[i]);
    }
    return ok ? kc : NULL;
}

/* Patch donor blob fields (mmap RW; offset table = the calibration slot for that HAL format) */
static bool donor_patch_blob(const struct ahb_calib* kc, int blob_fd,
                             uint32_t stride_px, uint32_t height, long size) {
    uint32_t* b = (uint32_t*)mmap(NULL, kc->blob_size,
                                  PROT_READ | PROT_WRITE, MAP_SHARED, blob_fd, 0);
    if (b == MAP_FAILED) {
        LOGE("donor blob mmap: %s", strerror(errno));
        return false;
    }
    for (int i = 0; i < kc->n_stride_px; i++)
        *(uint32_t*)((char*)b + kc->stride_px_off[i]) = stride_px;
    for (int i = 0; i < kc->n_stride_b; i++)
        *(uint32_t*)((char*)b + kc->stride_b_off[i]) = stride_px * 4;
    for (int i = 0; i < kc->n_height; i++)
        *(uint32_t*)((char*)b + kc->height_off[i]) = height;
    for (int i = 0; i < kc->n_size; i++)
        *(uint32_t*)((char*)b + kc->size_off[i]) = (uint32_t)size;
    for (int i = 0; i < kc->n_size_exact; i++)
        *(uint32_t*)((char*)b + kc->size_exact_off[i]) = (uint32_t)size;
    for (int i = 0; i < kc->n_extent; i++)
        *(uint32_t*)((char*)b + kc->extent_off[i]) = (uint32_t)(size + kc->extent_const);
    munmap(b, kc->blob_size);
    return true;
}

static AHardwareBuffer* wrap_dmabuf_ahb(const struct awl_bq_buffer* b,
                                        uint64_t usage, AHardwareBuffer* tmpl) {
    /* k_calibs global table shared by multiple render threads (first import per
     * format triggers one calibration) — hold the lock throughout: calibration
     * + patching read consistently. import includes gralloc calls (ms-scale)
     * but happens only on buffer replacement; serialization is acceptable */
    static std::mutex wrap_lock;
    std::lock_guard<std::mutex> wlk(wrap_lock);

    /* All supported dmabuf formats map to a single HAL format (see
     * import_dmabuf_texture); the caller may not carry the constant. */
    uint32_t hal = AWL_HAL_BGRA_8888;
    struct ahb_calib* kc = ahb_calibrate(hal);
    if (!kc) {
        LOGE("blob offsets not calibrated — dmabuf import refused (no fallback)");
        return NULL;
    }

    /* donor: the surface's CURRENT AHB when its geometry matches (steady
     * state — blob already patched for this geometry, only the pixel fd
     * changes, no allocation). Otherwise a transient 4x4 donor patched to the
     * target geometry (first buffer / resize; EXP-B verified on device),
     * released before returning — the forged AHB's own fd[1] dup keeps the
     * blob alive. A live tmpl blob is never re-patched: SF/kgsl may still hold
     * the era's AHBs. */
    const native_handle* nd = NULL;
    AHardwareBuffer* donor = NULL;   /* non-NULL = transient cold-path donor */
    if (tmpl) {
        AHardwareBuffer_Desc td = {};
        AHardwareBuffer_describe(tmpl, &td);
        const native_handle* tnh = AHardwareBuffer_getNativeHandle(tmpl);
        if (tnh && tnh->numFds == 2 && tnh->numInts == kc->num_ints &&
            td.width == b->width && td.height == b->height &&
            td.stride == b->stride / 4 && td.format == hal)
            nd = tnh;
    }
    if (!nd) {
        AHardwareBuffer_Desc dd = {};
        dd.width = 4; dd.height = 4;
        dd.format = hal; dd.layers = 1;
        dd.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
        if (AHardwareBuffer_allocate(&dd, &donor) != 0) {
            LOGE("donor allocation failed");
            return NULL;
        }
        nd = AHardwareBuffer_getNativeHandle(donor);
        if (!nd || nd->numFds != 2 || nd->numInts != kc->num_ints) {
            LOGE("donor layout drift (numFds=%d numInts=%d/%d)",
                 nd ? nd->numFds : -1, nd ? nd->numInts : -1, kc->num_ints);
            AHardwareBuffer_release(donor);
            return NULL;
        }
        if (!donor_patch_blob(kc, nd->data[1], b->stride / 4, b->height,
                              (long)b->stride * b->height)) {
            AHardwareBuffer_release(donor);
            return NULL;
        }
    }

    /* handle ints: donor template + target geometry. tmpl path: the template
     * already carries exactly this geometry — verbatim copy. Cold path: the
     * fresh 4x4 donor's ints still describe 4x4 — patch to the target. */
    int ints[64];
    memcpy(ints, &nd->data[2], (size_t)kc->num_ints * 4);
    if (donor) {
        ints[kc->idx_stride_px] = (int)(b->stride / 4);
        for (int i = 0; i < kc->n_idx_height; i++)
            ints[kc->idx_height[i]] = (int)b->height;
        ints[kc->idx_width] = (int)b->width;
        ints[kc->idx_size] = (int)((long)b->stride * b->height);
        ints[kc->idx_stride_b] = (int)b->stride;
    }

    /* GraphicBuffer::flatten wire (libs/ui/GraphicBuffer.cpp):
     * 13-int header + handle ints in the stream, numFds fds via SCM_RIGHTS.
     * Independent high-bit id namespace (bit 63): AOSP-allocated GraphicBuffer
     * ids occupy (pid << 32) | seq in this process — a same-format forged id
     * colliding with a real one made SF's buffer cache hit different layers'
     * buffers as one entry (HWC era). Bit 63 keeps the spaces disjoint. */
    static std::atomic<uint32_t> counter{0};
    uint64_t id = ((uint64_t)getpid() << 32) | (counter++ & 0xffffffffu)
                | (1ull << 63);
    int32_t head[13];
    head[0] = 0x47423031;                 /* 'GB01' */
    head[1] = (int32_t)b->width;
    head[2] = (int32_t)b->height;
    head[3] = (int32_t)(b->stride / 4);   /* declared stride = container's true row pitch (px) */
    head[4] = (int32_t)hal;
    head[5] = 1;                          /* layerCount */
    head[6] = (int32_t)usage;
    head[7] = (int32_t)(id >> 32);
    head[8] = (int32_t)id;
    head[9] = 0;                          /* generationNumber */
    head[10] = 2;                         /* numFds: pixel + blob */
    head[11] = kc->num_ints;
    head[12] = (int32_t)(usage >> 32);

    int pix = dup(b->dmabuf_fd);          /* container dmabuf */
    int blb = dup(nd->data[1]);           /* donor metadata blob */
    if (pix < 0 || blb < 0) {
        LOGE("dup: %s", strerror(errno));
        if (pix >= 0) close(pix);
        if (blb >= 0) close(blb);
        AHardwareBuffer_release(donor);
        return NULL;
    }

    size_t total = (13 + kc->num_ints) * 4;
    int32_t* wire = (int32_t*)malloc(total);
    memcpy(wire, head, sizeof(head));
    memcpy(wire + 13, ints, (size_t)kc->num_ints * 4);

    int sv[2];
    AHardwareBuffer* out = NULL;
    int rc = -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
        char cbuf[CMSG_SPACE(2 * sizeof(int))];
        struct iovec iov = { wire, total };
        struct msghdr msg = {};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(2 * sizeof(int));
        int fds[2] = { pix, blb };
        memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));
        msg.msg_controllen = cmsg->cmsg_len;

        if (sendmsg(sv[0], &msg, 0) >= 0)
            rc = AHardwareBuffer_recvHandleFromUnixSocket(sv[1], &out);
        else
            LOGE("sendmsg: %s", strerror(errno));
        close(sv[0]);
        close(sv[1]);
    } else {
        LOGE("socketpair: %s", strerror(errno));
    }
    free(wire);
    close(pix);
    close(blb);                           /* kernel already handed over to the peer */

    if (rc != 0 || !out) {
        LOGE("recvHandleFromUnixSocket rc=%d — importBuffer refused "
             "(%ux%u stride=%u hal=%u)", rc, b->width, b->height, b->stride, hal);
        if (out) AHardwareBuffer_release(out);
        if (donor) AHardwareBuffer_release(donor);
        return NULL;
    }
    /* transient cold-path donor: released now — the blob survives through the
     * relay (out's handle holds its own fd dup) */
    if (donor) AHardwareBuffer_release(donor);
    return out;
}

/* ---------------- per-surface dmabuf slot (public API) ---------------- */

static int awl_renderer_ahb_swap(awl_ahb_slot* s, const struct awl_bq_buffer* b) {
    /* identity = the dmabuf inode, fstat'ed once when the frame was queued
     * (awl_bq_buffer.ino) instead of per frame here; 0 = unknown there
     * (broken fd) → per-call fstat fallback */
    uint64_t ino = b->ino;
    if (!ino) {
        struct stat st;
        if (fstat(b->dmabuf_fd, &st) != 0) return -1;
        ino = (uint64_t)st.st_ino;
    }
    /* (The write-fence gate that used to sit here — the client's paint may
     * still be in flight at commit, resize-ack frames read as zeros for a
     * few ms — is now the queue's job: gethead returned this element only
     * after its acquire fence signaled, explicit or exported from the
     * dma-buf's own write fences.) */

    if (s->ahb && s->ino == ino &&
        s->w == b->width && s->h == b->height && s->stride == b->stride)
        return 0;   /* same dma-buf: the AHB already wraps this memory */

    /* dmabuf changed → re-forge (the old AHB itself is the donor when its
     * geometry matches — one gralloc import, zero allocations), then release
     * it: the release closes the swapped-out dmabuf fd, and the new AHB's own
     * blob-fd dup keeps the metadata alive. Consumers still displaying the
     * old buffer hold their own refs (SF) / their own image ref (EGL) — valid
     * until they re-import. */
    AHardwareBuffer* ahb = wrap_dmabuf_ahb(
        b, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, s->ahb);
    if (!ahb) return -1;   /* old AHB kept — retry next frame */
    if (s->ahb) AHardwareBuffer_release(s->ahb);
    s->ahb = ahb;
    s->ino = ino;
    s->w = b->width;
    s->h = b->height;
    s->stride = b->stride;
    return 1;
}

static void awl_renderer_ahb_slot_destroy(awl_ahb_slot* s) {
    if (s->ahb) AHardwareBuffer_release(s->ahb);
    memset(s, 0, sizeof(*s));
}

static void destroy_dmabuf_texture(wl_tex* t) {
    if (t->image != EGL_NO_IMAGE_KHR) {
        g.eglDestroyImageKHR(g.display, t->image);
        t->image = EGL_NO_IMAGE_KHR;
    }
    awl_renderer_ahb_slot_destroy(&t->slot);
}

/* HAL BGRA_8888 constant lives in awl_renderer.hpp */

static void tex_params_default(void) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static bool import_dmabuf_texture(wl_tex* t, const struct awl_bq_buffer* b) {
    /* HAL format: DRM AR24/XR24 memory order B,G,R,(A|X) → HAL BGRA_8888 — the
     * GPU samples the buffer's true channel order, the R/B fix lives in the
     * texture descriptor instead of the shader (no u_swap_rb). XR24 alpha = the
     * X byte: harmless for blend-off layer 0 (Xwayland root); XR24 as a blended
     * child layer is not a real client pattern (chrome subsurfaces are AR24).
     * Sampled-only usage (texture) — GPU_FRAMEBUFFER not declared */
    int r = awl_renderer_ahb_swap(&t->slot, b);
    if (r <= 0) return r == 0;   /* same dma-buf: the texture IS that memory */

    /* dmabuf swapped → re-import: the old EGLImage pinned the old memory
     * through the swap via its own ref; drop it and build on the new AHB */
    if (t->image != EGL_NO_IMAGE_KHR) {
        g.eglDestroyImageKHR(g.display, t->image);
        t->image = EGL_NO_IMAGE_KHR;
    }

    /* eglGetNativeClientBufferANDROID: AHB → EGLClientBuffer (the sanctioned path) */
    EGLClientBuffer cb = g.eglGetNativeClientBuffer(t->slot.ahb);
    if (!cb) {
        LOGE("eglGetNativeClientBuffer == NULL");
        return false;
    }
    EGLImageKHR img = g.eglCreateImageKHR(g.display, EGL_NO_CONTEXT,
                                          EGL_NATIVE_BUFFER_ANDROID, cb, NULL);
    if (img == EGL_NO_IMAGE_KHR) {
        LOGE("eglCreateImageKHR(native buffer): 0x%x (%ux%u stride=%u)",
             eglGetError(), b->width, b->height, b->stride);
        return false;
    }
    LOGD("AHB import ok %ux%u stride=%u fd=%d",
         b->width, b->height, b->stride, b->dmabuf_fd);

    if (t->texture) glDeleteTextures(1, &t->texture);
    glGenTextures(1, &t->texture);
    glBindTexture(GL_TEXTURE_2D, t->texture);
    tex_params_default();
    g.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
    GLenum terr = glGetError();
    if (terr != GL_NO_ERROR) LOGE("EGLImageTargetTexture: 0x%x", terr);
    t->image = img;
    t->tex_is_image = true;
    t->tex_w = b->width;
    t->tex_h = b->height;
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
     * dying mid-frame cannot free it), lock it for the whole composite (the
     * head must not move under the GPU), drain superseded frames back to
     * the client, then take the newest complete head (waits its acquire
     * fence, bounded). NULL / NULL-marker = nothing to draw for this layer.
     * All logic-layer rwl-taking calls (get_layers / xform above, presented
     * below) stay outside the locked span — a surface teardown flushing a
     * queue under rwl.wr must never wait on a frame that needs rwl. */
    struct awl_bufferqueue* q[AWL_MAX_LAYERS + 1];
    const struct awl_bq_buffer* head[AWL_MAX_LAYERS + 1];
    bool more = false;   /* a queue still holds a newer, not-yet-complete frame */
    for (int i = 0; i < n; i++) {
        q[i] = awl_surface_queue_ref(lay[i].surface_id);
        head[i] = NULL;
        if (!q[i]) continue;
        awl_bufferqueue_lock(q[i]);
        awl_bufferqueue_drain(q[i]);
        head[i] = awl_bufferqueue_gethead(q[i], 100);
        if (awl_bufferqueue_pending(q[i]) > 0) more = true;
    }

    uint64_t seen[AWL_MAX_LAYERS + 1];
    int nseen = 0;
    bool drew = false;
    for (int i = 0; i < n; i++) {
        const struct awl_bq_buffer* b = head[i];
        if (!b || b->dmabuf_fd < 0) continue;   /* no frame yet / detached (NULL marker) */
        wl_tex& t = w->layers[lay[i].surface_id];   /* layers seen this frame */
        seen[nseen++] = lay[i].surface_id;
        bool ok = import_dmabuf_texture(&t, b);
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
        for (int i = 0; i < n; i++) {
            if (!q[i]) continue;
            awl_bufferqueue_arm(q[i]);
            awl_bufferqueue_unlock(q[i]);
            awl_bufferqueue_unref(q[i]);
        }
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
            if (it->second.texture) glDeleteTextures(1, &it->second.texture);
            it = w->layers.erase(it);
        } else {
            ++it;
        }
    }

    /* Release fence for the client buffers this composite sampled: a native
     * fence inserted after the draws (signals when the GPU is done reading;
     * the dup flushes the command stream). Attached to every locked head —
     * the queue hands it to the client as the explicit-sync release fence,
     * or parks it in the dma-buf's reservation for implicit-sync clients
     * (bq_release_cb). From that point the FENCE orders the client's reuse
     * after our read, so the queues are unlocked BEFORE eglSwapBuffers:
     * with swap interval 1 the swap blocks until vsync, and a lock held
     * across it makes every commit-time drain fail → the client only gets
     * buffers back once per vsync → vsync × buffer-count fps (measured:
     * vkmark pinned at 180). Without native fences the only guarantee left
     * is glFinish (GPU idle = reads done) before unlocking. No glFinish
     * otherwise: eglSwapBuffers submits with its own native fence (SF waits
     * GPU-side before scanout), frame_done goes out at submit time so the
     * client's next frame overlaps this one's GPU composite. */
    glDisable(GL_BLEND);
    int rel = frame_release_fence();
    if (rel < 0) glFinish();
    for (int i = 0; i < n; i++) {
        if (!q[i]) continue;
        if (head[i] && head[i]->dmabuf_fd >= 0)
            awl_bufferqueue_set_release_fence(q[i], rel);
        awl_bufferqueue_arm(q[i]);   /* pending incomplete frame: its fence, not the next vsync, releases the head */
        awl_bufferqueue_unlock(q[i]);
        awl_bufferqueue_unref(q[i]);
    }
    if (rel >= 0) close(rel);
    /* a newer frame arrived while we drew but was not complete yet: present
     * it as soon as the swap returns (the waiter above hands buffers back,
     * this re-arm keeps the screen current) */
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
