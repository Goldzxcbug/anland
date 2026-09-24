/* awl_sc.cpp — SurfaceControl compositor backend (HWC path), see awl_sc.hpp.
 *
 * Layer SCs are created straight from the window (ASurfaceControl_
 * createFromWindow siblings — the device-verified shape): the window Surface
 * itself is the root of the layer tree and every wayland layer (root surface,
 * subsurfaces, popups, drag icon, cursor) is one sibling; wl stacking order is
 * pure setZOrder order over the siblings. SurfaceFlinger/HWC composites the
 * siblings; the daemon composites nothing.
 *
 * Every layer runs in one of three modes, decided per latched frame:
 *   SCANOUT  the client's dma-buf itself, forged into an AHardwareBuffer
 *            (awl_ahb), is latched with setBuffer — zero copy, HWC plane.
 *   EGL      the client's dma-buf is sampled by this backend's GLES context
 *            (forged AHB → EGLImage texture; the GPU does not care about the
 *            pitch) into a platform-allocated swapchain buffer attached to
 *            the SC — one GPU blit for THIS layer only.
 *   EGL_SHM  wl_shm content: the client's pool is uploaded straight into a
 *            GL texture (damage rect only, awl_surface_shm_begin/end — the
 *            logic layer neither copies nor queues shm) and blitted into the
 *            swapchain buffer like EGL.
 *
 * HWC scan-out precondition (QCOM SDM, source + device 2026-09-17): the
 * display HAL rebuilds a layer buffer's plane layout from its aligned width
 * through gralloc's own stride rule and gives THAT pitch to DRM — a buffer
 * whose real pitch is not a fixed point of the rule (e.g. 1024 px, which
 * gralloc pads to 1280) fails ADDFB2 and its plane shows nothing, while GPU
 * composition (and screencap) render it fine. SCANOUT is therefore gated by
 * awl_ahb_hwc_scanout_ok; everything else goes through a platform-allocated
 * target (gralloc picks the layout — trivially valid for HWC and for SF's
 * own GPU fallback).
 *
 * Two owners, one lock (sc_window::m):
 *
 *   event path (awl_sc_sync, from window_dirty on the client's dispatch /
 *   input threads, and from attach): owns the LAYER SC LIFECYCLE. It
 *   snapshots the logic layer's stack (get_layers + cursor_layer — the same
 *   kwin below→surface→above traversal the hit-test uses), creates an SC for
 *   every layer that entered the stack, retires the SC of every layer that
 *   left it, and reassigns z = traversal index, all in ONE transaction. So an
 *   SC lives exactly as long as its surface is part of an attached window's
 *   tree — created the moment get_subsurface/get_popup/set_cursor links it,
 *   gone the moment the surface/role/link dies — never a frame later, never
 *   from the render thread. It never touches GL: GL objects a retired layer
 *   owned are handed to the render thread (g_gc).
 *
 *   render thread (choreographer vsync loop, one for all windows, owner of
 *   the backend's GLES context): owns only BUFFER STATE. Per layer: queue
 *   lock → drain → tryhead (never waits: an unsignaled acquire fence leaves
 *   the previous buffer AND its geometry on screen; the fence is watched in
 *   the looper and the layer re-checked the moment it signals, or at the
 *   next vsync — one client's GPU must not stall every window) → arm →
 *   unlock; a NEW frame is latched in its mode. No dmabuf frame → the shm
 *   source is asked. The layer geometry (position/scale/crop/transform/
 *   opacity) is applied in the SAME transaction as the buffer: it is a
 *   function of the latched buffer's dimensions and wl_surface.commit is
 *   atomic — buffer and geometry of one commit must land in one SF frame,
 *   so a pass that cannot latch the newest frame does not move the layer
 *   either (the logic layer's geometry already describes that frame).
 *
 * Pacing (measured against the GL path with vkmark, 2026-09-17):
 *   - frame_done goes out at the vsync TICK for every visible layer, from
 *     the render thread — not from SF's OnComplete (one vsync later: a
 *     frame-callback-paced client then ran at 29 fps) and not at the off-tick
 *     apply (a callback per vsync is the contract).
 *   - a commit wakes the render thread for one off-tick pass per window per
 *     interval, so a paced client's frame reaches SF's next composition
 *     instead of waiting for our tick (GL-path latency parity). A GPU
 *     client's frame is still on the GPU at that moment; the pass then
 *     watches its acquire fence (looper fd) and the apply happens when the
 *     fence signals — the interval's off-tick slot is consumed by the pass
 *     that applies, not by the one that found nothing latchable.
 *   - a client that keeps running ahead of the vsync (mailbox/immediate) is
 *     copied (EGL mode) instead of scanned out: zero-copy would leave it
 *     with no free buffer (SF holds three, the queue head a fourth) and one
 *     frame per vsync (123 fps vs 1300 copying). See sc_rlayer::overspeed.
 *
 * Layer removal contract (NDK surface_control.h): ASurfaceControl_release
 * only drops our reference — "the surface and its children may remain on
 * display as long as their parent remains on display". Under a live
 * SurfaceView parent a released-but-not-reparented layer keeps showing its
 * last buffer forever (device dumpsys 2026-09-17: generations of
 * `handleNotAlive` awl-layers stacked at z=1..3 under the SurfaceView). A
 * layer therefore leaves through hide + reparent(NULL) in a transaction, and
 * the reference is dropped only after that transaction is applied.
 *
 * Release chains — whatever is latched on an SC stays referenced until SF is
 * done with it:
 *   SCANOUT: the queue element. 36+ (dlsym) setBufferWithRelease gives a
 *   per-buffer OnRelease carrying the release fence → awl_bufferqueue_put
 *   (elem, fence) → bq_release_cb (esync fenced_release / implicit dma-buf
 *   reservation / wl_buffer.release). 29..35: the transaction's OnComplete
 *   stats give the PREVIOUS buffer's release fence per SC — each setBuffer
 *   (B2 over B1) parks B1 in the transaction context.
 *   EGL/EGL_SHM: the swapchain slot, same two mechanisms (slot goes back to
 *   its pool with SF's fence, GPU-waited before it is drawn into again). The
 *   source element of an EGL frame is put with the blit's native fence right
 *   away — the client gets its buffer back as soon as the GPU read it.
 * Contexts hold only acquired ASurfaceControls, queue-referenced elements,
 * shared_ptrs to pools and surface ids — no window pointers, a detached
 * window cannot dangle them. */
#include "awl_sc.hpp"
#include "awl.h"
#include "awl_ahb.hpp"
#include "awl_gl.hpp"
#include "awl_geom.h"
#include "awl_bufferqueue.h"

#include <android/api-level.h>
#include <android/choreographer.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_window.h>
#include <android/surface_control.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#define AWL_TAG "anland-sc"
#include "awl_log.h"

/* SCANOUT buffers need GPU_SAMPLED_IMAGE (SF may composite them; EGL-mode
 * sources are sampled by us) + COMPOSER_OVERLAY (HWC candidate; the forged
 * handle bakes the usage into its ints). */
#define AWL_SC_USAGE (AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | \
                      AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY)

/* wl_output.transform (0..7) → ANativeWindowTransform (native_window.h).
 * wl: how the compositor must transform the buffer for display; the Android
 * transform is the same family of axis permutations — the bijection covering
 * all 8. On-device check rides the transform-using clients (Xwayland). */
static const int32_t k_wl_to_android_xform[8] = {
    ANATIVEWINDOW_TRANSFORM_IDENTITY,                              /* 0 normal */
    ANATIVEWINDOW_TRANSFORM_ROTATE_90,                              /* 1 90 */
    ANATIVEWINDOW_TRANSFORM_ROTATE_180,                             /* 2 180 */
    ANATIVEWINDOW_TRANSFORM_ROTATE_270,                             /* 3 270 */
    ANATIVEWINDOW_TRANSFORM_MIRROR_HORIZONTAL,                      /* 4 flipped */
    ANATIVEWINDOW_TRANSFORM_MIRROR_HORIZONTAL | ANATIVEWINDOW_TRANSFORM_ROTATE_90,   /* 5 flipped_90 */
    ANATIVEWINDOW_TRANSFORM_MIRROR_VERTICAL,                        /* 6 flipped_180 */
    ANATIVEWINDOW_TRANSFORM_MIRROR_VERTICAL | ANATIVEWINDOW_TRANSFORM_ROTATE_90,    /* 7 flipped_270 */
};

/* Viewport source region (buffer-normalized u0,v0,su,sv) → layer-space crop
 * rect, normalized. Derived as the inverse of the GL sample matrix
 * (awl_gl_xform: buffer_uv = M · display_q → the region's display-space
 * preimage is Mᵀ·region — every case is an axis permutation/mirror). */
static void sample_crop_rect(int t, double u0, double v0, double su, double sv,
                             double* x0, double* y0, double* x1, double* y1) {
    double bu0 = u0, bv0 = v0, bu1 = u0 + su, bv1 = v0 + sv;
    switch (t & 7) {
    case 1:  *x0 = 1 - bv1; *y0 = bu0;  *x1 = 1 - bv0; *y1 = bu1;  return;
    case 2:  *x0 = 1 - bu1; *y0 = 1 - bv1; *x1 = 1 - bu0; *y1 = 1 - bv0; return;
    case 3:  *x0 = bv0;     *y0 = 1 - bu1; *x1 = bv1;    *y1 = 1 - bu0; return;
    case 4:  *x0 = 1 - bu1; *y0 = bv0;  *x1 = 1 - bu0; *y1 = bv1;  return;
    case 5:  *x0 = bv0;     *y0 = bu0;  *x1 = bv1;    *y1 = bu1;  return;
    case 6:  *x0 = bu0;     *y0 = 1 - bv1; *x1 = bu1; *y1 = 1 - bv0; return;
    case 7:  *x0 = 1 - bv1; *y0 = 1 - bu1; *x1 = 1 - bv0; *y1 = 1 - bu0; return;
    default: *x0 = bu0;     *y0 = bv0;  *x1 = bu1;    *y1 = bv1;  return;
    }
}

/* ---------------- API level / dlsym ---------------- */

typedef void (*ASurfaceTransaction_setBufferWithRelease_fn)(
    ASurfaceTransaction*, ASurfaceControl*, AHardwareBuffer*, int, void*,
    ASurfaceTransaction_OnBufferRelease);

static struct {
    int sdk = 0;
    ASurfaceTransaction_setBufferWithRelease_fn set_buffer_with_release = nullptr;
} g_api;

static void api_init(void) {
    if (g_api.sdk) return;
    g_api.sdk = android_get_device_api_level();
    if (g_api.sdk >= 36) {
        g_api.set_buffer_with_release =
            (ASurfaceTransaction_setBufferWithRelease_fn)dlsym(
                RTLD_DEFAULT, "ASurfaceTransaction_setBufferWithRelease");
        LOGI("setBufferWithRelease (36): %s",
             g_api.set_buffer_with_release ? "available" : "dlsym miss — stats mode");
    }
    LOGI("SC backend: sdk=%d", g_api.sdk);
}

/* ---------------- GL garbage (render-thread destruction) ----------------
 * GL objects orphaned off the render thread — a layer retired on the event
 * path, a swapchain whose last reference dropped on an SF callback thread —
 * are destroyed here, on the render thread with its context current. */
static std::mutex g_gc_lock;
static std::vector<std::function<void()>> g_gc;

static void gc_push(std::function<void()> f) {
    std::lock_guard<std::mutex> lk(g_gc_lock);
    g_gc.push_back(std::move(f));
}

static void gc_run(void) {
    std::vector<std::function<void()>> v;
    {
        std::lock_guard<std::mutex> lk(g_gc_lock);
        v.swap(g_gc);
    }
    for (auto& f : v) f();
}

/* ---------------- EGL-mode target swapchain ----------------
 * Platform-allocated buffers (gralloc picks the layout) each imported as an
 * EGLImage texture = the blit's FBO color attachment. Shared between the
 * layer record and the release contexts SF fires for its buffers; the last
 * reference may drop on an SF thread, so GL handles go through g_gc. */
struct sc_egl_pool {
    /* SF's pipeline holds up to three of these at once — the one on screen,
     * the one latched for the next present, and the one whose release fence
     * it only reports one frame after replacement — plus the slot being
     * rendered into here. Three slots skipped every other frame. */
    static constexpr int SLOTS = 4;
    struct slot {
        AHardwareBuffer* ahb = nullptr;
        awl_gl_tex tex;
        bool queued = false;          /* latched on the SC: SF owns it until its release */
        int release_fd = -1;          /* SF's release fence of the last use (GPU-waited before reuse) */
    } s[SLOTS];
    uint32_t w = 0, h = 0;
    std::mutex m;                     /* slot state: render thread ↔ SF callback threads */

    ~sc_egl_pool() {
        for (auto& sl : s) {
            if (sl.release_fd >= 0) close(sl.release_fd);
            awl_gl_tex t = sl.tex;
            AHardwareBuffer* a = sl.ahb;
            if (t.texture || t.image != EGL_NO_IMAGE_KHR || a)
                gc_push([t, a]() mutable {
                    awl_gl_tex_release(&t);
                    if (a) AHardwareBuffer_release(a);
                });
        }
    }
    /* SF dropped this slot's buffer; fence = when its reads end (-1 = already) */
    void release(int idx, int fence) {
        std::lock_guard<std::mutex> lk(m);
        slot& sl = s[idx];
        if (sl.release_fd >= 0) close(sl.release_fd);
        sl.release_fd = fence >= 0 ? fcntl(fence, F_DUPFD_CLOEXEC, 0) : -1;
        sl.queued = false;
    }
    /* a slot SF does not own (-1 = none: SF is two frames behind, skip this
     * vsync); its pending release fence is handed out for a GPU-side wait */
    int acquire(int* fence_out) {
        std::lock_guard<std::mutex> lk(m);
        for (int i = 0; i < SLOTS; i++) {
            if (s[i].queued) continue;
            *fence_out = s[i].release_fd;
            s[i].release_fd = -1;
            return i;
        }
        *fence_out = -1;
        return -1;
    }
    void mark_queued(int idx) {
        std::lock_guard<std::mutex> lk(m);
        s[idx].queued = true;
    }
};

/* render thread, context current */
static std::shared_ptr<sc_egl_pool> pool_create(uint32_t w, uint32_t h) {
    auto p = std::make_shared<sc_egl_pool>();
    p->w = w;
    p->h = h;
    for (auto& sl : p->s) {
        AHardwareBuffer_Desc d = {};
        d.width = w;
        d.height = h;
        d.layers = 1;
        d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;   /* GL renders RGBA; SF/HWC scan it out */
        d.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                  AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
        if (AHardwareBuffer_allocate(&d, &sl.ahb) != 0 || !sl.ahb) {
            LOGE("EGL-mode target %ux%u: AHardwareBuffer_allocate failed", w, h);
            return nullptr;   /* destructor releases the rest */
        }
        if (!awl_gl_tex_import(sl.ahb, &sl.tex)) {
            LOGE("EGL-mode target %ux%u: EGLImage import failed", w, h);
            return nullptr;
        }
    }
    return p;
}

/* 36+: OnRelease of a swapchain slot (SF thread). Context owns a pool ref. */
struct sc_egl_ctx {
    std::shared_ptr<sc_egl_pool> pool;
    int slot;
};
static void sc_on_egl_release(void* context, int release_fence_fd) {
    sc_egl_ctx* c = (sc_egl_ctx*)context;
    c->pool->release(c->slot, release_fence_fd);
    if (release_fence_fd >= 0) close(release_fence_fd);
    delete c;
}

/* ---------------- transaction context (self-contained) ---------------- */

struct sc_rel {                        /* stats mode: one retiring buffer of an SC */
    ASurfaceControl* sc;               /* acquired (outlives window teardown) */
    struct awl_bq_buffer* elem;        /* SCANOUT: referenced element awaiting SF's fence */
    std::shared_ptr<sc_egl_pool> pool; /* EGL*: swapchain slot */
    int slot;
};

struct sc_txn {
    std::vector<sc_rel> retiring;      /* previous buffers of this txn's setBuffers */
};

/* OnComplete (SF thread): previous-buffer release fences (29..35 stats
 * mode). frame_done is NOT sent from here: SF's completion runs one vsync
 * after our apply, and a client paced by wl_surface.frame (Mesa FIFO) would
 * only manage one frame per two vsyncs (measured 29 fps at 60 Hz). The
 * render thread sends frame_done at its vsync tick instead (kwin: frame
 * callbacks go out at the compositor's repaint, not at scan-out). */
static void sc_on_complete(void* context, ASurfaceTransactionStats* stats) {
    sc_txn* t = (sc_txn*)context;
    for (const sc_rel& r : t->retiring) {
        int fence = ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, r.sc);
        if (r.elem) awl_bufferqueue_put(r.elem, fence);   /* put dup/merges */
        if (r.pool) r.pool->release(r.slot, fence);
        if (fence >= 0) close(fence);
        ASurfaceControl_release(r.sc);
    }
    delete t;
}

/* OnRelease (SF thread, 36+): this exact client buffer is reusable — its
 * element goes back with SF's fence. Context = the element itself (the
 * queue keeps it alive until the put). */
static void sc_on_buffer_release(void* context, int release_fence_fd) {
    struct awl_bq_buffer* e = (struct awl_bq_buffer*)context;
    awl_bufferqueue_put(e, release_fence_fd);
    if (release_fence_fd >= 0) close(release_fence_fd);
}

/* ---------------- per-layer record ---------------- */

enum sc_mode { SC_MODE_NONE = 0, SC_MODE_SCANOUT, SC_MODE_EGL, SC_MODE_EGL_SHM };
static const char* mode_name(sc_mode m) {
    switch (m) {
    case SC_MODE_SCANOUT: return "SCANOUT";
    case SC_MODE_EGL:     return "EGL";
    case SC_MODE_EGL_SHM: return "EGL_SHM";
    default:              return "none";
    }
}

struct sc_rlayer {
    uint64_t surface_id;
    ASurfaceControl* sc = nullptr;     /* created by the event-path sync */
    int64_t z = -1;                    /* stack index last pushed via setZOrder */

    /* buffer state (render thread; guarded by the window's m) */
    sc_mode mode = SC_MODE_NONE;       /* what the SC shows right now */
    /* NOTE: current (SCANOUT) and egl_onscreen (EGL*) are provably exclusive —
     * latched_forget_locked clears both before every latch — but pooling them
     * into a union saves ~4B and obscures the mode switch; not done. */
    struct awl_bq_buffer* current = nullptr;   /* SCANOUT: element latched (referenced) */
    std::shared_ptr<sc_egl_pool> pool;         /* EGL*: target swapchain */
    int egl_onscreen = -1;                     /* EGL*: pool slot latched on the SC */
    uint64_t egl_seq = 0;                      /* EGL*: identity of the source rendered into it
                                                * (element seq / shm serial) */
    struct awl_ahb_cache* ahb = nullptr;       /* forge cache: SCANOUT handles / EGL source textures */
    awl_gl_shm_tex shm;                        /* EGL_SHM: upload texture */
    uint32_t bw = 0, bh = 0, bfmt = 0;         /* latched content: buffer dims + fourcc (geometry inputs) */
    unsigned stall = 0;                        /* consecutive vsyncs the head was incomplete */
    unsigned skips = 0;                        /* EGL frames skipped for want of a free slot */

    /* Client pacing (tick-sampled from the queue's superseded counter).
     * A client that keeps committing faster than the vsync — mailbox /
     * immediate present modes, benchmarks — cannot be scanned out zero-copy
     * without starving itself: SF holds three of its buffers (latched, on
     * screen, release reported one frame late) and the queue's newest head
     * a fourth, which is every buffer a Mesa mailbox swapchain has; it then
     * gets one back per vsync (measured: vkmark 123 fps here vs 1300 on the
     * copying GL path). Such a layer is presented through an EGL-mode copy
     * instead: SF holds our pool slots, the client's buffer goes back the
     * moment the GPU read it. Paced clients (frame callbacks / FIFO — every
     * browser and toolkit) never trip this and stay zero-copy. Hysteresis:
     * ~¼ s of dropped frames to enter, 2 s of none to leave. */
    unsigned sup_last = 0;                     /* queue superseded counter at the last tick */
    unsigned fast_ticks = 0, slow_ticks = 0;   /* consecutive ticks with / without dropped frames */
    /* 1-bit group — every access on the render path under sc_window::m
     * (bit-field writes are word-wide read-modify-write: one word, one lock) */
    bool overspeed : 1;                        /* client commits faster than vsync → EGL copy mode */
    bool has_buffer : 1;                       /* a buffer is latched + visible */
    bool geo_valid : 1;                        /* last applied geometry below is meaningful */
    bool gopaque : 1;
    bool gcrop : 1;
    /* last applied geometry (skip unchanged) */
    int32_t gx = 0, gy = 0, gw = 0, gh = 0;
    int32_t gxform = -1;
    int32_t gcrop_l = 0, gcrop_t = 0, gcrop_r = 0, gcrop_b = 0;
};

struct sc_window {
    uint64_t id;
    ANativeWindow* nw = nullptr;       /* self-held reference: the parent of
                                        * every layer SC (createFromWindow) */
    std::mutex m;                      /* layers + teardown */
    std::map<uint64_t, std::unique_ptr<sc_rlayer>> layers;
    std::atomic<bool> dead{false};
    std::atomic<bool> kick{false};     /* window_dirty since the last pass */
    std::atomic<bool> applied_now{false};   /* an off-tick pass APPLIED a transaction in this vsync interval */
    bool watch_armed = false;          /* render thread only: an acquire-fence watch is registered (watch_arm) */
    uint64_t frame_clock = 0;          /* AHB LRU clock */
};

/* ---------------- globals / threads ----------------
 * Pacing: the vsync tick is the frame boundary — every attached window gets
 * a pass, and every visible layer its frame_done, once per tick. A
 * window_dirty between ticks (a commit) additionally wakes the render
 * thread for ONE off-tick pass per window per interval, so a paced client's
 * frame reaches SF at its next composition instead of waiting for our tick
 * (the GL path renders on commit; without this the SC path added a frame
 * of latency). When that pass finds the frame's acquire fence pending it
 * applies nothing and arms a fence watch (watch_arm); the watch's pass is
 * the one that applies. Off-tick passes send no frame_done: a frame
 * callback per vsync is the contract, and a buffer applied off-tick may
 * still be superseded by the tick's before SF latches either. */

static std::mutex g_map_lock;
static std::map<uint64_t, std::shared_ptr<sc_window>> g_windows;

static std::atomic<bool> g_running{false};
static std::thread g_render_th;
static std::atomic<ALooper*> g_lo{nullptr};
static AChoreographer* g_ch = nullptr;

/* the render thread's GLES context (EGL-mode layers); created on first need */
static struct {
    bool tried = false, ok = false;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface pbuf = EGL_NO_SURFACE;
    awl_gl_quad quad;
    GLuint fbo = 0;
} g_gl;

static std::vector<std::shared_ptr<sc_window>> snapshot_windows(void) {
    std::vector<std::shared_ptr<sc_window>> v;
    std::lock_guard<std::mutex> lk(g_map_lock);
    for (auto& kv : g_windows) v.push_back(kv.second);
    return v;
}

static std::shared_ptr<sc_window> find_window(uint64_t id) {
    std::lock_guard<std::mutex> lk(g_map_lock);
    auto it = g_windows.find(id);
    return it == g_windows.end() ? nullptr : it->second;
}

/* ---------------- acquire-fence watch (render thread looper) ----------------
 * A frame the pass cannot latch because its acquire fence is still pending
 * — the normal state right after a GPU client's commit, i.e. at every kick
 * pass — would otherwise wait for the next tick: one frame of latency the
 * GL path does not have (it renders on commit, waiting the fence inline).
 * The pass registers that fence in the render thread's looper instead; when
 * it signals, the window is kicked and an off-tick pass latches the frame.
 * One watch per window at a time — the pass re-arms for whatever is still
 * pending. The fd is a private dup: ALooper_removeFd BEFORE close, the epoll
 * interest is keyed by the open file description the client shares (same
 * rule as the bufferqueue's own waiter). */
struct sc_watch {
    std::weak_ptr<sc_window> w;
};

static int sc_watch_cb(int fd, int events, void* data) {
    (void)events;   /* INPUT = signaled; ERROR/HANGUP = dead fence: the pass treats both as complete */
    sc_watch* ctx = (sc_watch*)data;
    if (ALooper* lo = g_lo.load()) ALooper_removeFd(lo, fd);
    close(fd);
    if (auto w = ctx->w.lock()) {
        w->watch_armed = false;
        w->kick.store(true);   /* pollOnce returns POLL_CALLBACK → sc_render_kicked */
    }
    delete ctx;
    return 0;   /* already removed above */
}

/* render thread; takes ownership of fd */
static void watch_arm(const std::shared_ptr<sc_window>& w, int fd) {
    ALooper* lo = g_lo.load();
    sc_watch* ctx = (lo && !w->watch_armed) ? new sc_watch{ w } : nullptr;
    if (!ctx || ALooper_addFd(lo, fd, 0, ALOOPER_EVENT_INPUT, sc_watch_cb, ctx) != 1) {
        delete ctx;
        close(fd);
        return;
    }
    w->watch_armed = true;
}

/* ---------------- GL context (render thread) ---------------- */

static bool gl_ensure(void) {
    if (g_gl.tried) return g_gl.ok;
    g_gl.tried = true;
    if (!awl_gl_init()) return false;
    g_gl.ctx = awl_gl_create_context();
    if (g_gl.ctx == EGL_NO_CONTEXT) return false;
    if (!awl_gl_make_current_offscreen(g_gl.ctx, &g_gl.pbuf)) {
        eglDestroyContext(awl_gl_display(), g_gl.ctx);
        g_gl.ctx = EGL_NO_CONTEXT;
        return false;
    }
    if (!awl_gl_quad_create(&g_gl.quad)) return false;
    glGenFramebuffers(1, &g_gl.fbo);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    g_gl.ok = true;
    const char* glv = (const char*)glGetString(GL_VERSION);
    LOGI("SC EGL-mode context up (%s)", glv ? glv : "?");
    return true;
}

static void gl_teardown(void) {   /* render thread exit */
    gc_run();
    if (g_gl.ok) {
        awl_gl_quad_destroy(&g_gl.quad);
        glDeleteFramebuffers(1, &g_gl.fbo);
    }
    if (g_gl.ctx != EGL_NO_CONTEXT) {
        eglMakeCurrent(awl_gl_display(), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_gl.pbuf != EGL_NO_SURFACE) eglDestroySurface(awl_gl_display(), g_gl.pbuf);
        eglDestroyContext(awl_gl_display(), g_gl.ctx);
    }
    g_gl = {};
}

/* ---------------- layer SC lifecycle (event path) ----------------
 * Everything below runs with the window lock held and batches into one
 * transaction: retire + create + z. Never touches GL. */

struct sc_sync_txn {
    ASurfaceTransaction* txn = nullptr;
    sc_txn* ctx = nullptr;
    std::vector<ASurfaceControl*> dropped;   /* released after apply */

    ASurfaceTransaction* get(void) {
        if (!txn) {
            txn = ASurfaceTransaction_create();
            ctx = new sc_txn();
        }
        return txn;
    }
    /* Apply (if anything was queued) and drop the retired SC references —
     * after the apply, so the reparent(NULL) travels on a live handle. */
    void finish(void) {
        if (txn) {
            if (!ctx->retiring.empty())
                ASurfaceTransaction_setOnComplete(txn, ctx, sc_on_complete);
            else
                delete ctx;
            ASurfaceTransaction_apply(txn);
            ASurfaceTransaction_delete(txn);
            txn = nullptr;
            ctx = nullptr;
        }
        for (ASurfaceControl* sc : dropped) ASurfaceControl_release(sc);
        dropped.clear();
    }
};

/* The SC's buffer is about to be replaced or hidden: park what it shows for
 * the release SF reports with THIS transaction's stats (29..35; ctx = the
 * transaction's context). In 36-mode every latched buffer already carries
 * its own OnRelease context — just forget it. */
static void latched_forget_locked(sc_txn* ctx, sc_rlayer* L) {
    if (ctx && L->sc && !g_api.set_buffer_with_release) {
        if (L->current) {
            ASurfaceControl_acquire(L->sc);
            ctx->retiring.push_back({ L->sc, L->current, nullptr, -1 });
        }
        if (L->egl_onscreen >= 0 && L->pool) {
            ASurfaceControl_acquire(L->sc);
            ctx->retiring.push_back({ L->sc, nullptr, L->pool, L->egl_onscreen });
        }
    } else if (!L->sc && L->current && !g_api.set_buffer_with_release) {
        awl_bufferqueue_put(L->current, -1);   /* never latched anywhere */
    }
    L->current = nullptr;
    L->egl_onscreen = -1;
}

/* The layer left the window's tree: hide + reparent(NULL) through the
 * transaction (see the file header for why release alone is not removal),
 * park the latched buffer, hand GL-owned state to the render thread. */
static void layer_retire_locked(sc_sync_txn& st, sc_rlayer* L) {
    if (L->sc) {
        ASurfaceTransaction* txn = st.get();
        ASurfaceTransaction_setVisibility(txn, L->sc, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
        ASurfaceTransaction_reparent(txn, L->sc, nullptr);
        latched_forget_locked(st.ctx, L);
        st.dropped.push_back(L->sc);
        L->sc = nullptr;
    } else {
        latched_forget_locked(nullptr, L);
    }
    if (L->ahb || L->shm.texture) {
        struct awl_ahb_cache* c = L->ahb;
        awl_gl_shm_tex shm = L->shm;
        gc_push([c, shm]() mutable {
            if (c) awl_ahb_cache_destroy(c);
            awl_gl_shm_release(&shm);
        });
        L->ahb = nullptr;
        L->shm = awl_gl_shm_tex();
    }
    L->pool.reset();   /* GL handles → g_gc from the destructor (SF may still hold slots via their contexts) */
    L->has_buffer = false;
    L->mode = SC_MODE_NONE;
}

/* Reconcile the window's SC set with the logic layer's CURRENT stack. The
 * only place that creates, retires or reorders layer SCs. Called on every
 * window_dirty (topology mutations, commits, cursor changes all end there)
 * and at attach — cheap when nothing changed (snapshot + map walk, no
 * transaction). */
static void sc_sync_window(const std::shared_ptr<sc_window>& w) {
    awl_layer_info_t lay[AWL_MAX_LAYERS + 1];
    int n = awl_surface_get_layers(w->id, lay, AWL_MAX_LAYERS);
    if (n < 0) n = 0;
    if (n > 0 && awl_pointer_cursor_layer(w->id, &lay[n])) n++;   /* cursor: topmost */

    std::unique_lock<std::mutex> lk(w->m);
    if (w->dead || !w->nw) return;

    sc_sync_txn st;

    /* layers that left the stack */
    for (auto it = w->layers.begin(); it != w->layers.end();) {
        bool found = false;
        for (int i = 0; i < n && !found; i++)
            found = lay[i].surface_id == it->first;
        if (!found) {
            LOGD("window %llu: layer %llu left the stack — SC retired",
                 (unsigned long long)w->id, (unsigned long long)it->first);
            layer_retire_locked(st, it->second.get());
            it = w->layers.erase(it);
        } else {
            ++it;
        }
    }

    /* layers that entered / moved */
    bool z_dirty = false;
    for (int i = 0; i < n; i++) {
        auto it = w->layers.find(lay[i].surface_id);
        if (it == w->layers.end()) {
            auto L = std::make_unique<sc_rlayer>();
            L->surface_id = lay[i].surface_id;
            it = w->layers.emplace(lay[i].surface_id, std::move(L)).first;
        }
        sc_rlayer* L = it->second.get();
        if (!L->sc) {
            L->sc = ASurfaceControl_createFromWindow(w->nw, "awl-layer");
            if (!L->sc) {
                LOGE("window %llu: createFromWindow(layer %llu) failed — retried on the next dirty",
                     (unsigned long long)w->id, (unsigned long long)L->surface_id);
                continue;
            }
            L->z = -1;
            z_dirty = true;
        }
        if (L->z != i) {
            L->z = i;
            z_dirty = true;
        }
    }
    if (z_dirty) {
        ASurfaceTransaction* txn = st.get();
        for (int i = 0; i < n; i++) {
            auto it = w->layers.find(lay[i].surface_id);
            if (it != w->layers.end() && it->second->sc)
                ASurfaceTransaction_setZOrder(txn, it->second->sc, (int32_t)i);
        }
        LOGD("window %llu: z pass (n=%d)", (unsigned long long)w->id, n);
    }
    st.finish();
}

/* ---------------- latching (render thread, window lock held) ---------------- */

static void mode_set(sc_window* w, sc_rlayer* L, sc_mode m, const char* why) {
    if (L->mode != m)
        LOGI("window %llu layer %llu: %s → %s (%s)", (unsigned long long)w->id,
             (unsigned long long)L->surface_id, mode_name(L->mode), mode_name(m), why);
    L->mode = m;
}

/* Everything a new latch has in common: the previous buffer retires, the
 * layer shows if it was hidden, geometry re-applies against the new dims. */
static void latched_common(sc_txn* ctx, ASurfaceTransaction* txn, sc_rlayer* L,
                           uint32_t bw, uint32_t bh, uint32_t bfmt) {
    (void)ctx;
    if (!L->has_buffer)
        ASurfaceTransaction_setVisibility(txn, L->sc, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    L->has_buffer = true;
    L->bw = bw;
    L->bh = bh;
    L->bfmt = bfmt;
    L->geo_valid = false;
}

/* SCANOUT: the client element itself, forged. false = forge refused (retry
 * next vsync; the head reference is put back). */
static bool latch_scanout(sc_window* w, sc_rlayer* L, ASurfaceTransaction* txn, sc_txn* ctx,
                          struct awl_bq_buffer* head) {
    if (!L->ahb) L->ahb = awl_ahb_cache_create(awl_gl_tex_payload_destroy);
    struct awl_ahb_slot* slot =
        L->ahb ? awl_ahb_cache_get(L->ahb, head, AWL_SC_USAGE, w->frame_clock) : nullptr;
    if (!slot) {
        awl_bufferqueue_put(head, -1);
        return false;
    }
    latched_forget_locked(ctx, L);
    if (g_api.set_buffer_with_release)
        g_api.set_buffer_with_release(txn, L->sc, slot->ahb, -1, head, sc_on_buffer_release);
    else
        ASurfaceTransaction_setBuffer(txn, L->sc, slot->ahb, -1);   /* acquire fence already signaled */
    L->current = head;   /* ownership: latched on the SC (identity + stats-mode retire) */
    latched_common(ctx, txn, L, head->width, head->height, head->format);
    mode_set(w, L, SC_MODE_SCANOUT, "pitch is a gralloc fixed point");
    return true;
}

/* EGL / EGL_SHM: blit `tex` (bw×bh, the source's own pixel grid, 1:1 — the
 * target IS the source re-laid-out, so crop/transform/scale downstream are
 * untouched) into a swapchain slot and latch it with the blit's fence. The
 * fence is also returned dup'd (-1 = none) for the caller's source release.
 * false = no free slot / no target (nothing latched). */
static bool latch_egl(sc_window* w, sc_rlayer* L, ASurfaceTransaction* txn, sc_txn* ctx,
                      GLuint tex, uint32_t bw, uint32_t bh, uint32_t bfmt, sc_mode mode,
                      const char* why, int* rel_fence) {
    *rel_fence = -1;
    std::shared_ptr<sc_egl_pool> pool = L->pool;
    if (!pool || pool->w != bw || pool->h != bh) {
        pool = pool_create(bw, bh);   /* the old one lives on in SF's release contexts */
        if (!pool) return false;
    }
    int wait_fd = -1;
    int idx = pool->acquire(&wait_fd);
    if (idx < 0) {
        L->skips++;
        if (L->skips == 30 || (L->skips % 600) == 0)
            LOGE("window %llu layer %llu: no free EGL-mode slot (SF holds all %d) — frame skipped (#%u)",
                 (unsigned long long)w->id, (unsigned long long)L->surface_id,
                 sc_egl_pool::SLOTS, L->skips);
        return false;
    }
    awl_gl_wait_fence_fd(wait_fd);   /* SF's reads of this slot's previous content */

    glBindFramebuffer(GL_FRAMEBUFFER, g_gl.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           pool->s[idx].tex.texture, 0);
    glViewport(0, 0, (GLsizei)bw, (GLsizei)bh);
    glDisable(GL_BLEND);
    awl_gl_quad_begin(&g_gl.quad, (float)bw, (float)bh, -1.0f);
    const float dst[4] = { 0.f, 0.f, (float)bw, (float)bh };
    const float uv[4] = { 0.f, 0.f, 1.f, 1.f };
    awl_gl_quad_draw(&g_gl.quad, tex, dst, uv, 0);
    int fence = awl_gl_fence_fd();
    if (fence < 0) glFinish();   /* no native fences: completion by idling */
    else *rel_fence = fcntl(fence, F_DUPFD_CLOEXEC, 0);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
        LOGE("window %llu layer %llu: EGL-mode blit %ux%u gl error 0x%x",
             (unsigned long long)w->id, (unsigned long long)L->surface_id, bw, bh, err);

    latched_forget_locked(ctx, L);
    if (g_api.set_buffer_with_release)
        g_api.set_buffer_with_release(txn, L->sc, pool->s[idx].ahb, fence,
                                      new sc_egl_ctx{ pool, idx }, sc_on_egl_release);
    else
        ASurfaceTransaction_setBuffer(txn, L->sc, pool->s[idx].ahb, fence);
    /* (setBuffer took ownership of `fence`) */
    pool->mark_queued(idx);
    L->pool = pool;
    L->egl_onscreen = idx;
    latched_common(ctx, txn, L, bw, bh, bfmt);
    mode_set(w, L, mode, why);
    return true;
}

/* hide: the layer has no content any more */
static void latch_hide(ASurfaceTransaction* txn, sc_txn* ctx, sc_rlayer* L, bool* any) {
    if (!L->has_buffer) return;
    ASurfaceTransaction_setVisibility(txn, L->sc, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
    latched_forget_locked(ctx, L);
    L->has_buffer = false;
    L->mode = SC_MODE_NONE;
    *any = true;
}

/* ---------------- render frame (buffer state only) ----------------
 * tick = the vsync pass (pacing bookkeeping + frame_done collection); an
 * off-tick pass only latches. done: surfaces owed a frame_done (tick).
 * Returns whether a transaction was applied. */

static bool sc_render_window(const std::shared_ptr<sc_window>& w, int64_t vsync_id,
                             bool tick, std::vector<uint64_t>* done) {
    awl_layer_info_t lay[AWL_MAX_LAYERS + 1];
    int n = awl_surface_get_layers(w->id, lay, AWL_MAX_LAYERS);
    if (n <= 0) return false;
    if (awl_pointer_cursor_layer(w->id, &lay[n])) n++;
    awl_view_xform_t xf;
    awl_surface_get_view_xform(w->id, &xf);

    std::unique_lock<std::mutex> lk(w->m);
    if (w->dead || !w->nw) return false;

    w->frame_clock++;

    ASurfaceTransaction* txn = ASurfaceTransaction_create();
    sc_txn* ctx = new sc_txn();
    bool any = false;

    for (int i = 0; i < n; i++) {
        auto it = w->layers.find(lay[i].surface_id);
        if (it == w->layers.end()) continue;   /* entered after the last sync — next dirty creates it */
        sc_rlayer* L = it->second.get();
        if (!L->sc) continue;
        if (!tick && L->overspeed) continue;   /* runs ahead anyway: one latch per tick is all it gets */

        /* dmabuf source: drain superseded frames, take a referenced complete
         * head WITHOUT waiting (an incomplete head keeps the latched buffer
         * on screen; re-checked when its fence signals — watch_arm — or at
         * the next vsync), arm the waiter behind the head */
        struct awl_bq_buffer* head = nullptr;
        int pending = 0;
        int wfd = -1;   /* readiness fd of the frame this pass may fail to latch (watch_arm) */
        unsigned sup = L->sup_last;
        struct awl_bufferqueue* q = awl_surface_queue_ref(lay[i].surface_id);
        if (q) {
            awl_bufferqueue_lock(q);
            awl_bufferqueue_drain(q);
            head = awl_bufferqueue_tryhead(q);
            pending = awl_bufferqueue_count(q);
            if (pending > (head ? 1 : 0) && !w->watch_armed && !L->overspeed)
                wfd = awl_bufferqueue_incomplete_fd(q);
            awl_bufferqueue_arm(q);
            awl_bufferqueue_unlock(q);
            sup = awl_bufferqueue_superseded(q);
            awl_bufferqueue_unref(q);
        }
        /* hold: the client's NEWEST frame is not latchable this pass — the
         * head's own fence is pending (incomplete), or a complete head has
         * incomplete newer frame(s) behind it (drain stops at the first
         * unsignaled fence). lay[i] describes that newest commit. */
        const bool incomplete = !head && pending > 0;
        const bool hold = pending > (head ? 1 : 0);
        if (tick) {   /* pacing: did the client drop frames since the last tick? */
            bool dropped = sup != L->sup_last;
            L->sup_last = sup;
            if (dropped) { L->fast_ticks++; L->slow_ticks = 0; }
            else         { L->slow_ticks++; L->fast_ticks = 0; }
            if (!L->overspeed && L->fast_ticks >= 15) {
                L->overspeed = true;
                LOGI("window %llu layer %llu: client runs ahead of vsync — copy mode",
                     (unsigned long long)w->id, (unsigned long long)L->surface_id);
            } else if (L->overspeed && L->slow_ticks >= 120) {
                L->overspeed = false;
                LOGI("window %llu layer %llu: client paced again — zero-copy allowed",
                     (unsigned long long)w->id, (unsigned long long)L->surface_id);
            }
        }
        if (incomplete) {
            L->stall++;   /* the head exists but its writer is not done */
            if (L->stall == 60 || (L->stall % 600) == 0)
                LOGE("window %llu layer %llu: acquire fence pending for %u vsyncs — "
                     "keeping the previous buffer on screen",
                     (unsigned long long)w->id, (unsigned long long)L->surface_id, L->stall);
        } else {
            L->stall = 0;
        }
        if (head && head->dmabuf_fd < 0) {   /* NULL marker: no dmabuf frame from here on */
            awl_bufferqueue_put(head, -1);
            head = nullptr;
        }

        bool latched = false;   /* a new buffer went into this transaction */
        if (head) {
            /* ---- dmabuf frame ---- */
            if (L->shm.texture) awl_gl_shm_release(&L->shm);   /* the surface left wl_shm */
            bool scanout = awl_ahb_hwc_scanout_ok(head) && !L->overspeed;
            if (scanout) {
                if (L->mode == SC_MODE_SCANOUT && head == L->current) {
                    awl_bufferqueue_put(head, -1);   /* same element still current */
                } else {
                    latched = latch_scanout(w.get(), L, txn, ctx, head);
                }
            } else if (L->mode == SC_MODE_EGL && head->seq == L->egl_seq) {
                awl_bufferqueue_put(head, -1);       /* already blitted this frame */
            } else if (!gl_ensure()) {
                awl_bufferqueue_put(head, -1);
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    LOGE("SC EGL-mode context unavailable — layers HWC cannot scan out stay blank");
                }
            } else {
                if (!L->ahb) L->ahb = awl_ahb_cache_create(awl_gl_tex_payload_destroy);
                struct awl_ahb_slot* slot =
                    L->ahb ? awl_ahb_cache_get(L->ahb, head, AWL_SC_USAGE, w->frame_clock) : nullptr;
                GLuint tex = slot ? awl_gl_slot_texture(slot) : 0;
                int rel = -1;
                if (tex && latch_egl(w.get(), L, txn, ctx, tex, head->width, head->height,
                                     head->format, SC_MODE_EGL,
                                     L->overspeed ? "client runs ahead of vsync"
                                                  : "pitch is not a gralloc fixed point", &rel)) {
                    L->egl_seq = head->seq;
                    latched = true;
                }
                awl_bufferqueue_put(head, rel);   /* the GPU read is the release fence */
                if (rel >= 0) close(rel);
            }
        } else if (incomplete) {
            /* ---- dmabuf frame whose writer is not done ----
             * Keep what is on screen — buffer AND geometry (hold below) —
             * until the fence signals (watch) or the next tick. This is NOT
             * "no dmabuf frame": the shm branch would find no shm source on
             * a dmabuf client and hide the layer, i.e. every commit whose
             * GPU work is still running at the kick pass (all of them, for a
             * client that commits right after submitting) blanked the layer
             * for a frame and re-latched it at the tick — chrome scrolling
             * under explicit sync flickered/juddered while static content
             * (no commits) looked fine. */
        } else {
            /* ---- no dmabuf frame: wl_shm content? ---- */
            awl_shm_frame_t f;
            int r = awl_surface_shm_begin(lay[i].surface_id, L->shm.serial, &f);
            if (r == 1) {
                bool ok = gl_ensure() && awl_gl_shm_update(&L->shm, &f);
                awl_surface_shm_end(&f, ok);   /* consumed → the client gets wl_buffer.release */
                if (!ok) {
                    static bool warned = false;
                    if (!warned) {
                        warned = true;
                        LOGE("SC EGL_SHM upload failed — wl_shm layers cannot be shown");
                    }
                }
            } else if (r == 0) {
                if (L->shm.texture) awl_gl_shm_release(&L->shm);
                latch_hide(txn, ctx, L, &any);   /* unmapped */
                continue;
            }
            if (L->shm.texture &&
                (L->mode != SC_MODE_EGL_SHM || L->egl_seq != L->shm.serial)) {
                int rel = -1;
                if (latch_egl(w.get(), L, txn, ctx, L->shm.texture, L->shm.w, L->shm.h,
                              L->shm.format, SC_MODE_EGL_SHM, "wl_shm source", &rel)) {
                    L->egl_seq = L->shm.serial;
                    latched = true;
                }
                if (rel >= 0) close(rel);   /* shm was released at upload; the GPU owns the copy */
            }
        }
        if (wfd >= 0) {
            if (!latched) watch_arm(w, wfd);   /* wake for the frame we could not take */
            else close(wfd);
        }
        if (latched) any = true;
        if (!L->has_buffer) continue;
        if (tick && done) done->push_back(lay[i].surface_id);   /* visible this vsync → frame_done */

        /* ---- geometry (the GL renderer's math, via awl_geom.h) ----
         * Atomic with the buffer above: one commit = one SF transaction.
         * lay[i] is the logic layer's NEWEST state; when that commit's frame
         * is still pending (hold) its position/size must wait for it too —
         * moving the OLD buffer to the NEW position is a one-frame content/
         * position skew that a scrolling client shows as judder. The latch
         * re-applies geometry anyway (latched_common clears geo_valid). */
        if (hold && !latched) continue;
        double rsw, rsh;
        awl_layer_sampled(&lay[i], L->bw, L->bh, &rsw, &rsh);
        int32_t Wd = awl_snap_extent(lay[i].w, xf.sx, rsw);
        int32_t Hd = awl_snap_extent(lay[i].h, xf.sy, rsh);
        if (Wd <= 0 || Hd <= 0) continue;   /* logical size unknown (buffer resource gone): keep the last geometry */
        double scx = rsw > 0.0 ? (double)Wd / rsw : 1.0;
        double scy = rsh > 0.0 ? (double)Hd / rsh : 1.0;
        double X = ((double)lay[i].x - (double)xf.gox) * xf.sx + xf.ox;
        double Y = ((double)lay[i].y - (double)xf.goy) * xf.sy + xf.oy;
        int32_t atr = k_wl_to_android_xform[lay[i].transform & 7];
        bool opaque = L->bfmt == AWL_FOURCC_XRGB8888;
        bool has_crop = !(lay[i].u0 <= 0.0 && lay[i].v0 <= 0.0 &&
                          lay[i].su >= 1.0 && lay[i].sv >= 1.0);
        int32_t cl = 0, ct = 0, cr = 0, cb = 0;
        if (has_crop) {
            double x0, y0, x1, y1;
            sample_crop_rect(lay[i].transform & 7, lay[i].u0, lay[i].v0,
                             lay[i].su, lay[i].sv, &x0, &y0, &x1, &y1);
            /* layer-space axes: odd transforms carry the swapped buffer axes */
            double ax = (lay[i].transform & 1) ? (double)L->bh : (double)L->bw;
            double ay = (lay[i].transform & 1) ? (double)L->bw : (double)L->bh;
            cl = (int32_t)lround(x0 * ax);
            ct = (int32_t)lround(y0 * ay);
            cr = (int32_t)lround(x1 * ax);
            cb = (int32_t)lround(y1 * ay);
            /* setCrop clips in layer space, it does not move the clipped
             * region to the layer origin: SF's screen bounds are
             * transform(bufferBounds ∩ crop) (Layer::computeBounds), i.e.
             * the source region would land at position + crop.origin ×
             * scale. Pull the position back by that much so the viewport
             * source region sits where the logical rect says. */
            X -= (double)cl * scx;
            Y -= (double)ct * scy;
        }
        int32_t Xi = (int32_t)lround(X);
        int32_t Yi = (int32_t)lround(Y);
        if (!L->geo_valid || Xi != L->gx || Yi != L->gy || Wd != L->gw || Hd != L->gh ||
            atr != L->gxform || opaque != L->gopaque || has_crop != L->gcrop ||
            cl != L->gcrop_l || ct != L->gcrop_t || cr != L->gcrop_r || cb != L->gcrop_b) {
            ASurfaceTransaction_setPosition(txn, L->sc, Xi, Yi);
            ASurfaceTransaction_setScale(txn, L->sc, (float)scx, (float)scy);
            ASurfaceTransaction_setBufferTransform(txn, L->sc, atr);
            ASurfaceTransaction_setBufferTransparency(
                txn, L->sc, opaque ? ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE
                                   : ASURFACE_TRANSACTION_TRANSPARENCY_TRANSLUCENT);
            if (has_crop) {
                ARect rc = { cl, ct, cr, cb };
                ASurfaceTransaction_setCrop(txn, L->sc, rc);
            } else if (L->gcrop) {
                ARect none = { 0, 0, 0, 0 };   /* viewport source reset: empty crop = uncropped */
                ASurfaceTransaction_setCrop(txn, L->sc, none);
            }
            L->geo_valid = true;
            L->gx = Xi; L->gy = Yi; L->gw = Wd; L->gh = Hd;
            L->gxform = atr; L->gopaque = opaque;
            L->gcrop = has_crop;
            L->gcrop_l = cl; L->gcrop_t = ct; L->gcrop_r = cr; L->gcrop_b = cb;
            any = true;
        }
    }

    if (any) {
        if (!ctx->retiring.empty())
            ASurfaceTransaction_setOnComplete(txn, ctx, sc_on_complete);
        else
            delete ctx;
        if (vsync_id && g_api.sdk >= 33)
            ASurfaceTransaction_setFrameTimeline(txn, vsync_id);
        ASurfaceTransaction_apply(txn);
    } else {
        delete ctx;
    }
    ASurfaceTransaction_delete(txn);
    return any;
}

/* frame_done outside the window lock (takes rwl.rd + ev_lock, sends) */
static void send_frame_done(const std::vector<uint64_t>& done) {
    for (uint64_t sid : done) awl_surface_presented(sid);
}

/* vsync tick: every window, every visible layer's frame_done */
static void sc_render_all(int64_t vsync_id) {
    gc_run();   /* GL objects orphaned since the last frame (context current here) */
    std::vector<uint64_t> done;
    for (auto& w : snapshot_windows()) {
        if (w->dead.load()) continue;
        w->applied_now.store(false, std::memory_order_relaxed);   /* new interval */
        w->kick.store(false, std::memory_order_relaxed);
        done.clear();
        sc_render_window(w, vsync_id, true, &done);
        send_frame_done(done);
    }
}

/* off-tick (looper wake by awl_sc_kick, or a fence watch firing): kicked
 * windows that have not had their one off-tick APPLY this interval. A pass
 * that applied nothing — the commit's frame was still on the GPU — leaves
 * the slot to the pass the fence watch triggers. */
static void sc_render_kicked(void) {
    for (auto& w : snapshot_windows()) {
        if (w->dead.load()) continue;
        if (!w->kick.exchange(false)) continue;
        if (w->applied_now.load(std::memory_order_relaxed)) continue;
        if (sc_render_window(w, 0, false, nullptr))
            w->applied_now.store(true, std::memory_order_relaxed);
    }
}

/* ---------------- choreographer thread ---------------- */

static void sc_vsync_cb(const AChoreographerFrameCallbackData* data, void*) {
    if (!g_running.load(std::memory_order_relaxed)) return;
    int64_t id = 0;
    if (g_api.sdk >= 33) {
        size_t pref = AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(data);
        id = AChoreographerFrameCallbackData_getFrameTimelineVsyncId(data, pref);
    }
    sc_render_all(id);
    if (g_running.load(std::memory_order_relaxed))
        AChoreographer_postVsyncCallback(g_ch, sc_vsync_cb, nullptr);
}

static void sc_frame64_cb(int64_t, void*) {
    if (!g_running.load(std::memory_order_relaxed)) return;
    sc_render_all(0);
    if (g_running.load(std::memory_order_relaxed))
        AChoreographer_postFrameCallback64(g_ch, sc_frame64_cb, nullptr);
}

static void sc_render_thread(void) {
    g_lo.store(ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS));
    g_ch = AChoreographer_getInstance();
    if (!g_ch) {
        /* infra failure (e.g. the SF event connection refused — the sepolicy
         * rule set must carry surfaceflinger find + binder call, see
         * module/sepolicy.rule). Loud and dead: no transactions can ever be
         * paced; the daemon stays alive for the GL path via sc_enabled=0. */
        LOGE("AChoreographer_getInstance failed — SC render thread exiting "
             "(check awl_daemon → surfaceflinger sepolicy rules)");
        return;
    }
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = 2;   /* kwin DrmCommitThread::gainRealTime shape */
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
        LOGI("render thread: SCHED_FIFO denied (%s) — default policy", strerror(errno));
    LOGI("SC render thread up (choreographer=%p sdk=%d)", (void*)g_ch, g_api.sdk);
    if (g_api.sdk >= 33)
        AChoreographer_postVsyncCallback(g_ch, sc_vsync_cb, nullptr);
    else
        AChoreographer_postFrameCallback64(g_ch, sc_frame64_cb, nullptr);
    while (g_running.load(std::memory_order_relaxed)) {
        /* the choreographer's and the fence watches' fd callbacks run inside
         * pollOnce (the tick / a kick from sc_watch_cb → POLL_CALLBACK);
         * ALooper_wake from awl_sc_kick returns POLL_WAKE. Either way the
         * kicked windows get their off-tick pass. */
        int r = ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
        if ((r == ALOOPER_POLL_WAKE || r == ALOOPER_POLL_CALLBACK) &&
            g_running.load(std::memory_order_relaxed))
            sc_render_kicked();
    }
    gl_teardown();
    LOGI("SC render thread exiting");
}

static void threads_start(void) {
    static std::mutex start_lock;
    std::lock_guard<std::mutex> lk(start_lock);
    if (g_running.load()) return;
    api_init();
    g_running.store(true);
    g_render_th = std::thread(sc_render_thread);
}

/* ---------------- public API ---------------- */

static void sc_detach_internal(uint64_t id) {
    std::shared_ptr<sc_window> w;
    {
        std::lock_guard<std::mutex> lk(g_map_lock);
        auto it = g_windows.find(id);
        if (it == g_windows.end()) return;
        w = it->second;
        g_windows.erase(it);
    }
    w->kick.store(false);
    {
        std::lock_guard<std::mutex> lk(w->m);
        w->dead.store(true);
        /* every layer leaves through the same hide + reparent(NULL) path —
         * a re-SURFACE onto a still-live SurfaceView must not inherit the
         * previous generation's layers */
        sc_sync_txn st;
        for (auto& kv : w->layers)
            layer_retire_locked(st, kv.second.get());
        w->layers.clear();
        st.finish();
    }
    if (w->nw) {
        ANativeWindow_release(w->nw);
        w->nw = nullptr;
    }
    /* Transactions already applied keep their self-contained contexts; the
     * shared_ptr dies after the render thread drops its frame snapshot. */
}

int awl_sc_attach(uint64_t id, ANativeWindow* nw) {
    /* attach/detach for the same id fully serialized (renderer's pattern):
     * a re-attach first tears down whatever is there */
    static std::mutex attach_lock;
    std::lock_guard<std::mutex> alkg(attach_lock);
    sc_detach_internal(id);
    if (!nw) return 0;

    threads_start();
    ANativeWindow_acquire(nw);
    auto w = std::make_shared<sc_window>();
    w->id = id;
    w->nw = nw;
    {
        std::lock_guard<std::mutex> lk(g_map_lock);
        g_windows[id] = w;
    }
    sc_sync_window(w);   /* the current stack gets its SCs right here */
    LOGI("window %llu: SC attached (event-path layer tree)",
         (unsigned long long)id);
    return 0;
}

void awl_sc_kick(uint64_t id) {
    auto w = find_window(id);
    if (!w) return;
    w->kick.store(true);
    /* one off-tick pass per interval: a second commit in the same interval
     * waits for the tick (nothing to wake for) */
    if (!w->applied_now.load(std::memory_order_relaxed)) {
        if (ALooper* lo = g_lo.load()) ALooper_wake(lo);
    }
}

void awl_sc_sync(uint64_t id) {
    if (auto w = find_window(id)) sc_sync_window(w);
}

void awl_sc_shutdown(void) {
    std::vector<uint64_t> ids;
    {
        std::lock_guard<std::mutex> lk(g_map_lock);
        for (auto& kv : g_windows) ids.push_back(kv.first);
    }
    for (uint64_t id : ids) sc_detach_internal(id);
    if (g_running.exchange(false)) {
        if (ALooper* lo = g_lo.load()) ALooper_wake(lo);
        if (g_render_th.joinable()) g_render_th.join();
        g_lo.store(nullptr);
        g_ch = nullptr;
    }
}
