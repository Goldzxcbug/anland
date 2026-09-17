/* awl_sc.cpp — SurfaceControl compositor backend (HWC path), see awl_sc.hpp.
 *
 * Layer SCs are created straight from the window (ASurfaceControl_
 * createFromWindow siblings — the device-verified shape): the window Surface
 * itself is the root of the layer tree and every wayland layer (root surface,
 * subsurfaces, popups, drag icon, cursor) is one sibling; wl stacking order is
 * pure setZOrder order over the siblings. The daemon never composites: every
 * layer's queue head is forged into an AHardwareBuffer (awl_ahb) and latched
 * with one ASurfaceTransaction per window per vsync — SurfaceFlinger/HWC does
 * the rest. Frame source = the same per-surface bufferqueue the GL renderer
 * drains.
 *
 * Two owners, one lock (sc_window::m):
 *
 *   event path (awl_sc_sync, from window_dirty on the client's dispatch /
 *   input / shm threads, and from attach): owns the LAYER SC LIFECYCLE. It
 *   snapshots the logic layer's stack (get_layers + cursor_layer — the same
 *   kwin below→surface→above traversal the hit-test uses), creates an SC for
 *   every layer that entered the stack, retires the SC of every layer that
 *   left it, and reassigns z = traversal index, all in ONE transaction. So an
 *   SC lives exactly as long as its surface is part of an attached window's
 *   tree — created the moment get_subsurface/get_popup/set_cursor links it,
 *   gone the moment the surface/role/link dies — never a frame later, never
 *   from the render thread.
 *
 *   render thread (choreographer vsync loop, one for all windows): owns only
 *   BUFFER STATE. Per layer: queue lock → drain → tryhead (never waits: an
 *   unsignaled acquire fence leaves the previous buffer on screen and the
 *   layer is re-checked next vsync — one client's GPU must not stall every
 *   window) → arm → unlock; a NEW element forges/looks up its AHB and latches
 *   via setBuffer(sc, ahb, -1). The layer geometry (position/scale/crop/
 *   transform/opacity) is applied in the SAME transaction as the buffer: it
 *   is a function of the latched buffer's dimensions and wl_surface.commit is
 *   atomic — buffer and geometry of one commit must land in one SF frame,
 *   which is why geometry is not pushed from the event path.
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
 * Release chain: the element latched on an SC is kept referenced until SF is
 * done with it. On 36+ (dlsym) setBufferWithRelease gives a per-buffer
 * OnRelease callback carrying the release fence → awl_bufferqueue_put(elem,
 * fence) → the existing bq_release_cb (esync fenced_release / implicit
 * dma-buf reservation / wl_buffer.release). On 29..35 the transaction's
 * OnComplete stats provide the PREVIOUS buffer's release fence per SC: each
 * setBuffer(B2 over B1) parks B1 in the transaction context and the callback
 * puts it with the fence SF reported. The context holds only acquired
 * ASurfaceControls + queue-referenced elements + surface ids — no window
 * pointers, a detached window cannot dangle it. */
#include "awl_sc.hpp"
#include "awl.h"
#include "awl_ahb.hpp"
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
#include <math.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#define AWL_TAG "anland-sc"
#include "awl_log.h"

/* SC buffers need GPU_SAMPLED_IMAGE (SF may composite them) + COMPOSER_OVERLAY
 * (HWC candidate; forged handle bakes the usage into its ints). */
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
 * (k_root_xform: buffer_uv = M · display_q → the region's display-space
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

/* ---------------- transaction context (self-contained) ---------------- */

struct sc_rel {                        /* stats mode: one retiring buffer */
    ASurfaceControl* sc;               /* acquired (outlives window teardown) */
    struct awl_bq_buffer* elem;        /* referenced element awaiting SF's fence */
};

struct sc_txn {
    std::vector<sc_rel> retiring;      /* previous buffers of this txn's setBuffers */
    std::vector<uint64_t> presented;   /* frame_done at real presentation */
};

/* OnComplete (SF thread): previous-buffer release fences + frame_done. The
 * context owns nothing but its vectors; elements are queue-referenced, SCs
 * acquired. */
static void sc_on_complete(void* context, ASurfaceTransactionStats* stats) {
    sc_txn* t = (sc_txn*)context;
    for (const sc_rel& r : t->retiring) {
        int fence = ASurfaceTransactionStats_getPreviousReleaseFenceFd(stats, r.sc);
        if (fence >= 0) {
            awl_bufferqueue_put(r.elem, fence);
            close(fence);              /* put dup/merges, the returned fd is ours */
        } else {
            awl_bufferqueue_put(r.elem, -1);
        }
        ASurfaceControl_release(r.sc);
    }
    for (uint64_t sid : t->presented) awl_surface_presented(sid);
    delete t;
}

/* OnRelease (SF thread, 36+): this exact buffer is reusable — its element
 * goes back with SF's fence. Context = the element itself (the queue keeps
 * it alive until the put). */
static void sc_on_buffer_release(void* context, int release_fence_fd) {
    struct awl_bq_buffer* e = (struct awl_bq_buffer*)context;
    awl_bufferqueue_put(e, release_fence_fd);
    if (release_fence_fd >= 0) close(release_fence_fd);
}

/* ---------------- per-layer record ---------------- */

struct sc_rlayer {
    uint64_t surface_id;
    ASurfaceControl* sc = nullptr;     /* created by the event-path sync */
    int64_t z = -1;                    /* stack index last pushed via setZOrder */

    /* buffer state (render thread; guarded by the window's m) */
    struct awl_bq_buffer* current = nullptr;   /* element latched on the SC */
    struct awl_ahb_cache* ahb = nullptr;       /* per-layer forge cache */
    bool has_buffer = false;                   /* a buffer is latched+visible */
    unsigned stall = 0;                        /* consecutive vsyncs the head was incomplete */
    /* last applied geometry (skip unchanged) */
    bool geo_valid = false;
    int32_t gx = 0, gy = 0, gw = 0, gh = 0;
    int32_t gxform = -1;
    bool gopaque = false;
    bool gcrop = false;
    int32_t gcrop_l = 0, gcrop_t = 0, gcrop_r = 0, gcrop_b = 0;
};

struct sc_window {
    uint64_t id;
    ANativeWindow* nw = nullptr;       /* self-held reference: the parent of
                                        * every layer SC (createFromWindow) */
    std::mutex m;                      /* layers + teardown */
    std::map<uint64_t, std::unique_ptr<sc_rlayer>> layers;
    std::atomic<bool> dead{false};
    std::atomic<bool> kick{false};     /* window_dirty: force a txn this frame */
    uint64_t frame_clock = 0;          /* AHB LRU clock */
};

/* ---------------- globals / threads ---------------- */

static std::mutex g_map_lock;
static std::map<uint64_t, std::shared_ptr<sc_window>> g_windows;

static std::atomic<bool> g_running{false};
static std::thread g_render_th;
static ALooper* g_lo = nullptr;
static AChoreographer* g_ch = nullptr;

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

/* ---------------- layer SC lifecycle (event path) ----------------
 * Everything below runs with the window lock held and batches into one
 * transaction: retire + create + z. */

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

/* The layer left the window's tree: hide + reparent(NULL) through the
 * transaction (see the file header for why release alone is not removal),
 * hand back / park the latched element, drop the forge cache. Stats mode:
 * the element retires with THIS transaction's stats; 36-mode: its reference
 * belongs to its OnRelease context — SF fires it when the destroyed layer
 * drops the buffer. */
static void layer_retire_locked(sc_sync_txn& st, sc_rlayer* L) {
    if (L->sc) {
        ASurfaceTransaction* txn = st.get();
        ASurfaceTransaction_setVisibility(txn, L->sc, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
        ASurfaceTransaction_reparent(txn, L->sc, nullptr);
        if (L->current && !g_api.set_buffer_with_release) {
            ASurfaceControl_acquire(L->sc);
            st.ctx->retiring.push_back({ L->sc, L->current });
        }
        st.dropped.push_back(L->sc);
        L->sc = nullptr;
    } else if (L->current && !g_api.set_buffer_with_release) {
        awl_bufferqueue_put(L->current, -1);   /* never latched anywhere */
    }
    L->current = nullptr;
    if (L->ahb) {
        awl_ahb_cache_destroy(L->ahb);
        L->ahb = nullptr;
    }
    L->has_buffer = false;
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

/* ---------------- render frame (buffer state only) ---------------- */

static void sc_render_window(const std::shared_ptr<sc_window>& w, int64_t vsync_id) {
    awl_layer_info_t lay[AWL_MAX_LAYERS + 1];
    int n = awl_surface_get_layers(w->id, lay, AWL_MAX_LAYERS);
    if (n <= 0) return;
    if (awl_pointer_cursor_layer(w->id, &lay[n])) n++;
    awl_view_xform_t xf;
    awl_surface_get_view_xform(w->id, &xf);

    std::unique_lock<std::mutex> lk(w->m);
    if (w->dead || !w->nw) return;

    w->frame_clock++;
    bool kicked = w->kick.exchange(false);

    ASurfaceTransaction* txn = ASurfaceTransaction_create();
    sc_txn* ctx = new sc_txn();
    bool any = false;

    for (int i = 0; i < n; i++) {
        auto it = w->layers.find(lay[i].surface_id);
        if (it == w->layers.end()) continue;   /* entered after the last sync — next dirty creates it */
        sc_rlayer* L = it->second.get();
        if (!L->sc) continue;

        /* frame source: the layer's queue — drain superseded frames, take a
         * referenced complete head WITHOUT waiting (an incomplete head keeps
         * the latched buffer on screen; re-checked next vsync), arm the
         * waiter for the frame behind the head. */
        struct awl_bq_buffer* head = nullptr;
        int pending = 0;
        struct awl_bufferqueue* q = awl_surface_queue_ref(lay[i].surface_id);
        if (q) {
            awl_bufferqueue_lock(q);
            awl_bufferqueue_drain(q);
            head = awl_bufferqueue_tryhead(q);
            pending = awl_bufferqueue_count(q);
            awl_bufferqueue_arm(q);
            awl_bufferqueue_unlock(q);
            awl_bufferqueue_unref(q);
        }
        if (!head && pending > 0) {
            /* the head exists but its writer is not done: rate-limited
             * diagnostic (a fence that never signals would otherwise be a
             * silent frozen layer — the old blocking wait froze every window) */
            L->stall++;
            if (L->stall == 60 || (L->stall % 600) == 0)
                LOGE("window %llu layer %llu: acquire fence pending for %u vsyncs — "
                     "keeping the previous buffer on screen",
                     (unsigned long long)w->id, (unsigned long long)L->surface_id, L->stall);
        } else {
            L->stall = 0;
        }

        if (head && head->dmabuf_fd < 0) {
            /* NULL-marker: the layer unmaps — hide it, retire the latched
             * element (stats mode; the fence comes with this transaction's
             * stats — in 36-mode its OnRelease ctx owns the reference) */
            awl_bufferqueue_put(head, -1);
            if (L->has_buffer) {
                ASurfaceTransaction_setVisibility(
                    txn, L->sc, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
                if (L->current && !g_api.set_buffer_with_release) {
                    sc_rel r;
                    ASurfaceControl_acquire(L->sc);
                    r.sc = L->sc;
                    r.elem = L->current;
                    ctx->retiring.push_back(r);
                }
                L->current = nullptr;
                L->has_buffer = false;
                any = true;
            }
            continue;
        }

        if (!head) continue;    /* nothing committed yet / head incomplete */

        const struct awl_bq_buffer* sample = head;   /* what the SC shows after this txn */
        if (head != L->current) {
            if (!L->ahb) L->ahb = awl_ahb_cache_create(nullptr);
            struct awl_ahb_slot* slot =
                L->ahb ? awl_ahb_cache_get(L->ahb, head, AWL_SC_USAGE, w->frame_clock) : nullptr;
            if (!slot) {
                awl_bufferqueue_put(head, -1);   /* forge refused — retry next vsync */
                continue;
            }
            /* the replaced element retires through THIS transaction's
             * stats (29..35); in 36-mode each latched element's reference
             * was handed to its own OnRelease context at set time — nothing
             * to do for the old one here */
            if (L->current && !g_api.set_buffer_with_release) {
                sc_rel r;
                ASurfaceControl_acquire(L->sc);
                r.sc = L->sc;
                r.elem = L->current;
                ctx->retiring.push_back(r);
            }
            if (g_api.set_buffer_with_release)
                g_api.set_buffer_with_release(txn, L->sc, slot->ahb, -1, head,
                                              sc_on_buffer_release);
            else
                ASurfaceTransaction_setBuffer(txn, L->sc, slot->ahb, -1);
            if (!L->has_buffer)
                ASurfaceTransaction_setVisibility(
                    txn, L->sc, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
            L->current = head;          /* ownership: latched on the SC */
            L->has_buffer = true;
            L->geo_valid = false;       /* buffer change re-applies geometry */
            any = true;
            ctx->presented.push_back(lay[i].surface_id);
        } else {
            /* same element still current: keep it displayed, our extra
             * reference goes back; geometry (cursor moves, resize, zoom)
             * still applies against the latched buffer */
            sample = L->current;
            awl_bufferqueue_put(head, -1);
            ctx->presented.push_back(lay[i].surface_id);
        }

        /* ---- geometry (the GL renderer's math, via awl_geom.h) ----
         * Atomic with the buffer above: one commit = one SF transaction. */
        double rsw, rsh;
        awl_layer_sampled(&lay[i], sample->width, sample->height, &rsw, &rsh);
        int32_t Wd = awl_snap_extent(lay[i].w, xf.sx, rsw);
        int32_t Hd = awl_snap_extent(lay[i].h, xf.sy, rsh);
        double scx = rsw > 0.0 ? (double)Wd / rsw : 1.0;
        double scy = rsh > 0.0 ? (double)Hd / rsh : 1.0;
        double X = ((double)lay[i].x - (double)xf.gox) * xf.sx + xf.ox;
        double Y = ((double)lay[i].y - (double)xf.goy) * xf.sy + xf.oy;
        int32_t atr = k_wl_to_android_xform[lay[i].transform & 7];
        bool opaque = sample->format == AWL_FOURCC_XRGB8888;
        bool has_crop = !(lay[i].u0 <= 0.0 && lay[i].v0 <= 0.0 &&
                          lay[i].su >= 1.0 && lay[i].sv >= 1.0);
        int32_t cl = 0, ct = 0, cr = 0, cb = 0;
        if (has_crop) {
            double x0, y0, x1, y1;
            sample_crop_rect(lay[i].transform & 7, lay[i].u0, lay[i].v0,
                             lay[i].su, lay[i].sv, &x0, &y0, &x1, &y1);
            /* layer-space axes: odd transforms carry the swapped buffer axes */
            double ax = (lay[i].transform & 1) ? (double)sample->height : (double)sample->width;
            double ay = (lay[i].transform & 1) ? (double)sample->width : (double)sample->height;
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

    /* kicked but nothing changed: emit a no-op state transaction so its
     * OnComplete delivers the pending frame callbacks (GL parity — a dirty
     * window always presents). Target: the first buffered layer's SC. */
    if (!any && kicked) {
        ASurfaceControl* hit = nullptr;
        for (auto& kv : w->layers) {
            if (kv.second->has_buffer && kv.second->sc) {
                hit = kv.second->sc;
                ctx->presented.push_back(kv.first);
            } else if (kv.second->has_buffer) {
                ctx->presented.push_back(kv.first);
            }
        }
        if (hit) {
            ASurfaceTransaction_setVisibility(
                txn, hit, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
            any = true;
        }
    }

    if (any) {
        ASurfaceTransaction_setOnComplete(txn, ctx, sc_on_complete);
        if (vsync_id && g_api.sdk >= 33)
            ASurfaceTransaction_setFrameTimeline(txn, vsync_id);
        ASurfaceTransaction_apply(txn);
    } else {
        delete ctx;
    }
    ASurfaceTransaction_delete(txn);
}

static void sc_render_all(int64_t vsync_id) {
    for (auto& w : snapshot_windows()) {
        if (w->dead.load()) continue;
        sc_render_window(w, vsync_id);
    }
}

/* ---------------- choreographer thread ---------------- */

static void sc_vsync_cb(const AChoreographerFrameCallbackData* data, void*) {
    if (!g_running.load(std::memory_order_relaxed)) return;
    int64_t id = 0;
    if (g_api.sdk >= 33)
        id = AChoreographerFrameCallbackData_getFrameTimelineVsyncId(data, 0);
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
    g_lo = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
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
    while (g_running.load(std::memory_order_relaxed))
        ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
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
    if (auto w = find_window(id)) w->kick.store(true);
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
        if (g_lo) ALooper_wake(g_lo);
        if (g_render_th.joinable()) g_render_th.join();
        g_lo = nullptr;
        g_ch = nullptr;
    }
}
