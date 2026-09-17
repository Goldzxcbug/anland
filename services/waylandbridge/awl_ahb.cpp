/* awl_ahb.cpp — dmabuf → registered AHardwareBuffer (forging) + the generic
 * per-layer slot cache. Extracted verbatim from awl_renderer.cpp (the GL
 * renderer and the SurfaceControl compositor share the scheme).
 *
 * Donor-blob supplies metadata (device-verified scheme, see memory
 * snapalloc-ahb-construction):
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
#include "awl_ahb.hpp"
#include "awl_geom.h"

#include <android/log.h>
#include <sys/mman.h>             /* donor blob patching */
#include <sys/stat.h>             /* fstat: dma-buf inode identity */
#include <sys/socket.h>           /* socketpair / sendmsg / SCM_RIGHTS */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include <map>
#include <mutex>
#include <atomic>

#define AWL_TAG "anland-rd"
#include "awl_log.h"   /* LOGI/LOGE/LOGD (LOGD compiled out unless AWL_LOG_DEBUG) */

/* ---------------- platform stride oracle ----------------
 * gralloc's row pitch for a linear BGRA_8888 buffer of a given width under
 * the donor usage — measured, never modeled: on the OPD2513 (Adreno 840)
 * the rule pads pitches that land on 2 KiB multiples by 1 KiB (512→768,
 * 1024→1280, 1536→1792, 2048→2304, 3072→3328 px) but leaves 4096 alone —
 * an opaque libadreno_utils table. Probe = one 64-row allocation per width
 * (the pitch does not depend on the height: 64 vs 1808 rows measured equal),
 * released immediately; results cached for the daemon's lifetime. */
static std::mutex g_stride_lock;
static std::map<uint32_t, uint32_t> g_stride_cache;   /* width px → pitch px (0 = probe failed) */

uint32_t awl_ahb_platform_stride_px(uint32_t width_px) {
    if (!width_px) return 0;
    std::lock_guard<std::mutex> lk(g_stride_lock);
    auto it = g_stride_cache.find(width_px);
    if (it != g_stride_cache.end()) return it->second;
    AHardwareBuffer_Desc d = {};
    d.width = width_px;
    d.height = 64;
    d.layers = 1;
    d.format = AWL_HAL_BGRA_8888;
    d.usage = AWL_AHB_DONOR_USAGE;
    AHardwareBuffer* b = nullptr;
    uint32_t stride = 0;
    if (AHardwareBuffer_allocate(&d, &b) == 0 && b) {
        AHardwareBuffer_Desc got;
        AHardwareBuffer_describe(b, &got);
        stride = got.stride;
        AHardwareBuffer_release(b);
    } else {
        LOGE("stride oracle: probe allocation %ux64 failed — no scan-out verdict for this pitch",
             width_px);
    }
    g_stride_cache[width_px] = stride;
    LOGD("stride oracle: width %u px → pitch %u px", width_px, stride);
    return stride;
}

bool awl_ahb_hwc_scanout_ok(const struct awl_bq_buffer* b) {
    if (!b || b->stride % 4 != 0) return false;
    uint32_t spx = b->stride / 4;
    uint32_t want = awl_ahb_platform_stride_px(spx);
    return want == 0 /* oracle unavailable: no verdict, keep the HWC path */ || want == spx;
}

/* Official VNDK API (vndk/hardware_buffer.h); no header in the NDK sysroot,
 * symbol exported by libnativewindow.so (already linked via CMake) */
extern "C" const struct native_handle* AHardwareBuffer_getNativeHandle(
    const AHardwareBuffer* buffer);
struct native_handle { int version; int numFds; int numInts; int data[]; };

/* ---------------- calibration ---------------- */

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
/* One slot per HAL format (keyed lookup — concurrent consumers never evict
 * each other's calibration). Since the BGRA_8888 switch all dmabufs
 * (AR24/XR24) map to a single HAL format, in practice one slot covers
 * everything; the table stays generic for future formats. */
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
        dd.layers = 1; dd.usage = AWL_AHB_DONOR_USAGE;
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

AHardwareBuffer* awl_ahb_wrap(const struct awl_bq_buffer* b,
                              uint64_t usage, AHardwareBuffer* tmpl) {
    /* k_calibs global table shared by every consumer (first import per format
     * triggers one calibration) — hold the lock throughout: calibration +
     * patching read consistently. import includes gralloc calls (ms-scale)
     * but happens only on buffer replacement; serialization is acceptable */
    static std::mutex wrap_lock;
    std::lock_guard<std::mutex> wlk(wrap_lock);

    /* All supported dmabuf formats map to a single HAL format (see
     * awl_ahb_cache_get); the caller may not carry the constant. */
    uint32_t hal = AWL_HAL_BGRA_8888;
    struct ahb_calib* kc = ahb_calibrate(hal);
    if (!kc) {
        LOGE("blob offsets not calibrated — dmabuf import refused (no fallback)");
        return NULL;
    }

    /* donor: the consumer's CURRENT AHB when its geometry matches (steady
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
        dd.usage = AWL_AHB_DONOR_USAGE;
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

/* ---------------- per-layer slot cache ---------------- */

/* Forged buffers alive in the slot caches, process-wide. Each one pins two
 * fds; the bound is AWL_AHB_CACHE_SLOTS × live layers (a few dozen). The
 * canary fires long before the fd table (32768) does — a growing count is a
 * leak, and fd exhaustion is otherwise silent until every window is black. */
static std::atomic<int> g_forged_live{0};

static void forged_live_inc(void) {
    int n = g_forged_live.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n >= 512 && (n & (n - 1)) == 0)   /* 512, 1024, 2048, ... */
        LOGE("forged AHardwareBuffers alive: %d — leak? (2 fds each, table is 32768)", n);
}

static void forged_live_dec(void) {
    g_forged_live.fetch_sub(1, std::memory_order_relaxed);
}

struct awl_ahb_cache {
    void (*payload_destroy)(void*);
    struct awl_ahb_slot slot[AWL_AHB_CACHE_SLOTS];
};

struct awl_ahb_cache* awl_ahb_cache_create(void (*payload_destroy)(void*)) {
    struct awl_ahb_cache* c = new awl_ahb_cache();
    c->payload_destroy = payload_destroy;
    return c;
}

void awl_ahb_cache_destroy(struct awl_ahb_cache* c) {
    if (!c) return;
    for (struct awl_ahb_slot& s : c->slot) {
        if (s.payload && c->payload_destroy) c->payload_destroy(s.payload);
        if (s.ahb) {
            AHardwareBuffer_release(s.ahb);
            forged_live_dec();
        }
    }
    delete c;
}

int awl_ahb_forged_live(void) {
    return g_forged_live.load(std::memory_order_relaxed);
}

struct awl_ahb_slot* awl_ahb_cache_get(struct awl_ahb_cache* c,
                                       const struct awl_bq_buffer* b,
                                       uint64_t usage, uint64_t frame) {
    /* identity = the dmabuf inode, fstat'ed once when the frame was queued
     * (awl_bq_buffer.ino); 0 = unknown there (broken fd) → fstat here */
    uint64_t ino = b->ino;
    if (!ino) {
        struct stat st;
        if (fstat(b->dmabuf_fd, &st) != 0) return NULL;
        ino = (uint64_t)st.st_ino;
    }

    /* hit: same dma-buf, same geometry — the AHB IS that memory */
    struct awl_ahb_slot* victim = NULL;
    AHardwareBuffer* tmpl = NULL;   /* donor: any cached AHB of this geometry */
    for (struct awl_ahb_slot& s : c->slot) {
        if (s.ino == ino && s.w == b->width && s.h == b->height && s.stride == b->stride) {
            s.used = frame;
            return &s;
        }
        if (!s.ino) { if (!victim) victim = &s; }
        else {
            if (!tmpl && s.w == b->width && s.h == b->height && s.stride == b->stride) tmpl = s.ahb;
            if (!victim || (victim->ino && s.used < victim->used)) victim = &s;
        }
    }
    if (victim->ino && victim->ahb == tmpl) tmpl = NULL;   /* never donate from the entry being replaced (kept simple: cold path) */

    /* miss: forge an AHB over this dmabuf (HAL format: DRM AR24/XR24 memory
     * order B,G,R,(A|X) → HAL BGRA_8888 — the GPU samples the buffer's true
     * channel order, the R/B fix lives in the texture descriptor instead of
     * the shader. XR24 alpha = the X byte: for the SC path the layer goes
     * OPAQUE; XR24 as a translucent child layer is not a real client
     * pattern). The victim (LRU / empty) is replaced. */
    AHardwareBuffer* ahb = awl_ahb_wrap(b, usage, tmpl);
    if (!ahb) return NULL;   /* cache untouched — retry next frame */

    /* Evict: the payload (EGLImage/texture holding its own buffer reference)
     * first, then OUR reference on the forged buffer. Skipping the release
     * was the 2026-09-17 daemon-wide black screen: every eviction leaked the
     * forged handle's two fds (pixel dma-buf + snapalloc METADATA blob); a
     * client that commits a fresh dma-buf per frame (Xwayland) evicts every
     * frame → 32768 fds in minutes → every dup/socketpair/forge in the
     * process fails → no window can latch a buffer until the daemon
     * restarts. SF (setBuffer) and the GL driver (EGLImage) hold their own
     * references — releasing ours never pulls a buffer from under them. */
    if (victim->payload && c->payload_destroy) c->payload_destroy(victim->payload);
    if (victim->ahb) {
        AHardwareBuffer_release(victim->ahb);
        forged_live_dec();
    }
    victim->ahb = ahb;
    forged_live_inc();
    victim->ino = ino;
    victim->w = b->width;
    victim->h = b->height;
    victim->stride = b->stride;
    victim->used = frame;
    victim->payload = NULL;
    LOGD("AHB forged %ux%u stride=%u ino=%llu (cache slot %zd)",
         b->width, b->height, b->stride, (unsigned long long)ino,
         victim - c->slot);
    return victim;
}
