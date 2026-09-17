/* awl_ahb.hpp — dmabuf → registered AHardwareBuffer (forging) + the generic
 * per-layer AHB slot cache, shared by the GL renderer (EGLImage/texture
 * payload) and the SurfaceControl compositor (SF-consumed buffers).
 *
 * Forging = snapalloc donor-blob scheme (awl_renderer.cpp origin, AOSP
 * construction logic ported): the vendor gralloc requires a metadata blob
 * alongside the pixel dmabuf; offsets are self-calibrated at first use by
 * diffing two-geometry donors, then a donor blob is patched to the target
 * geometry and flattened over the wire (AHardwareBuffer_recvHandleFromUnix-
 * Socket). Refuses to forge when calibration fails — no fallback. */
#ifndef AWL_AHB_HPP
#define AWL_AHB_HPP

#include "awl_bufferqueue.h"

#include <android/hardware_buffer.h>
#include <stdint.h>

/* Forge a registered AHardwareBuffer over this dmabuf. usage =
 * AHARDWAREBUFFER_USAGE_* bits baked into the handle (the GL path samples it;
 * the SC path also declares COMPOSER_OVERLAY). tmpl = a live AHB of the same
 * geometry reused as donor (its blob is never re-patched; may be NULL).
 * Returns an acquired buffer (AHardwareBuffer_release when done) or NULL. */
AHardwareBuffer* awl_ahb_wrap(const struct awl_bq_buffer* b, uint64_t usage,
                              AHardwareBuffer* tmpl);

/* ---------------- per-layer slot cache ----------------
 * Identity = (ino, w, h, stride): a client cycling its swapchain hits the
 * cache from the second lap on — zero gralloc imports per frame. Capacity 8
 * covers every swapchain size in the wild (mesa WSI mailbox = 4, chrome ≤ 3);
 * a resize walks the LRU out. Each entry carries a consumer payload
 * (destroyed via the callback on eviction / cache destroy) — the GL renderer
 * hangs its EGLImage+texture there, the SC compositor its per-buffer state.
 * Single-consumer (one thread per cache); awl_ahb_wrap itself is globally
 * serialized internally (calibration + gralloc import are ms-scale). */

#define AWL_AHB_CACHE_SLOTS 8

struct awl_ahb_slot {
    AHardwareBuffer* ahb;    /* forged buffer, owned by the cache */
    uint64_t ino;            /* dma-buf inode identity */
    uint32_t w, h, stride;
    uint64_t used;           /* LRU clock (caller's frame counter) */
    void* payload;           /* consumer-owned (destroy callback) */
};

struct awl_ahb_cache;

struct awl_ahb_cache* awl_ahb_cache_create(void (*payload_destroy)(void* payload));
/* Destroys the cache and every payload (forged AHBs released). */
void awl_ahb_cache_destroy(struct awl_ahb_cache* c);

/* Resolve the slot for this frame's dmabuf: identity hit (payload intact,
 * used refreshed) or a forged miss (victim slot recycled — its payload
 * destroyed, payload = NULL for the caller to build). Returns NULL when the
 * forge was refused (cache untouched — retry next frame). ino == 0 in the
 * element is fstat'ed here. */
struct awl_ahb_slot* awl_ahb_cache_get(struct awl_ahb_cache* c,
                                       const struct awl_bq_buffer* b,
                                       uint64_t usage, uint64_t frame);

#endif /* AWL_AHB_HPP */
