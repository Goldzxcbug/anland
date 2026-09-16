/* awl_bufferqueue.h — per-surface buffer ring queue: the mediator between
 * the protocol dispatch thread (producer: commits) and the window's render
 * thread (consumer: frames).
 *
 * Pure Linux (dma-buf fd + sync_file fd + plain integers): no libwayland, no
 * Android objects — the logic layer feeds it, the adaptation layer drains it,
 * neither side sees the other's types. This is the seam a hardware-composer
 * backend plugs into later (it consumes the same elements).
 *
 * Model (mailbox):
 *   push    lock-free, any thread. Fails when the ring is full (the caller
 *           must hand the buffer straight back to its owner).
 *   drain   caller holds the head lock. Pops the head while the element
 *           behind it is already complete (its acquire fence signaled —
 *           completion is assumed FIFO: an element is complete only if every
 *           earlier one is), releasing every popped element through the
 *           release callback. Afterwards the head is the newest complete
 *           element (or the oldest incomplete one when none is complete yet).
 *   gethead caller holds the head lock. Returns the head after waiting for
 *           its acquire fence (bounded), NULL when empty. Valid until unlock.
 *   lock/trylock/unlock  head lock. The render thread locks around
 *           drain+gethead+draw-submit+set_release_fence and UNLOCKS BEFORE
 *           its vsync-blocking swap (holding it across the swap turns every
 *           producer into vsync × buffer-count — measured 180 fps with
 *           3 buffers at 60 Hz); the dispatch thread only trylocks at commit
 *           for an opportunistic drain (over-speed commits get their buffers
 *           back without waiting for a frame) and passes when the render
 *           thread holds it; the fence waiter (arm) blocks on it briefly.
 *
 * Elements own their fds: the queue closes dmabuf/acquire/release fds once the
 * release callback returned. `user` is an opaque producer cookie handed to the
 * callback (the logic layer keeps its wl_buffer/release-object bookkeeping
 * there). A dmabuf_fd of -1 is a NULL-buffer marker (wl_surface.attach(NULL)
 * commit): always "complete", the consumer draws nothing for that layer.
 *
 * Lifetime: refcounted. The owning surface holds one reference; a consumer
 * that resolved the queue takes its own for the duration of the frame, so a
 * surface dying mid-frame never frees a locked queue. The last unref releases
 * whatever is still queued. */
#ifndef AWL_BUFFERQUEUE_H
#define AWL_BUFFERQUEUE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct awl_bq_buffer {
    int dmabuf_fd;         /* owned; -1 = NULL-buffer marker */
    int acquire_fd;        /* sync_file the producer's writes signal; owned;
                            * -1 = none → implicit: the dma-buf's own write
                            * fences (poll POLLIN) gate readiness */
    int release_fd;        /* sync_file set by the consumer (set_release_fence);
                            * owned; -1 = none. Handed to the release callback
                            * (the callee may dup it) */
    uint64_t ino;          /* dma-buf inode: render-side identity (0 = unknown) */
    uint32_t width, height, stride;   /* stride in bytes */
    uint32_t format;       /* DRM fourcc */
    uint64_t modifier;
    void* user;            /* producer cookie (opaque to the queue) */
};

struct awl_bufferqueue;

/* Called for every element that leaves the queue (drain / push-fail path is
 * the caller's own / flush / last unref), with the queue lock NOT held by the
 * callback contract (drain holds the head lock — the callback must never
 * lock the same queue). fds are still open during the callback and closed by
 * the queue right after; the callee owns `user`. */
typedef void (*awl_bq_release_fn)(struct awl_bq_buffer* e, void* ctx);

struct awl_bufferqueue* awl_bufferqueue_create(awl_bq_release_fn fn, void* ctx);
void awl_bufferqueue_ref(struct awl_bufferqueue* q);
void awl_bufferqueue_unref(struct awl_bufferqueue* q);   /* last ref: flush + free */

/* Lock-free (single writer per call site, several producers may interleave).
 * 1 = queued (the queue now owns the fds + user), 0 = full (caller keeps
 * everything and must return the buffer itself). */
int awl_bufferqueue_push(struct awl_bufferqueue* q, const struct awl_bq_buffer* e);

int  awl_bufferqueue_trylock(struct awl_bufferqueue* q);   /* 1 = locked */
void awl_bufferqueue_lock(struct awl_bufferqueue* q);
void awl_bufferqueue_unlock(struct awl_bufferqueue* q);

/* Caller holds the lock. Returns the number of elements released. */
int awl_bufferqueue_drain(struct awl_bufferqueue* q);
/* Caller holds the lock. Waits up to timeout_ms for the head's acquire fence
 * (a timeout is logged and the head is returned anyway — never stall the
 * pipeline on a broken client fence). NULL = empty. */
const struct awl_bq_buffer* awl_bufferqueue_gethead(struct awl_bufferqueue* q,
                                                    int timeout_ms);
/* Caller holds the lock. Attach a consumer-side fence to the head (dup'd;
 * merged with one already there — the head may be sampled by several frames
 * before it is superseded). Once set, the consumer may unlock: the fence,
 * not the lock, now orders the producer's reuse after the consumer's read. */
void awl_bufferqueue_set_release_fence(struct awl_bufferqueue* q, int fence_fd);
/* Caller holds the lock; call after drain. If the frame behind the head is
 * still incomplete, watch its fence: a shared waiter thread drains the queue
 * (lock → drain → unlock) the moment it signals — a producer parked on
 * buffer starvation is unblocked when its own GPU work finishes, not at the
 * next commit or the next vsync. No-op when nothing is pending / already
 * armed. */
void awl_bufferqueue_arm(struct awl_bufferqueue* q);
/* Caller holds the lock. Release everything (surface teardown). */
void awl_bufferqueue_flush(struct awl_bufferqueue* q);

/* Lock-free snapshots (any thread; advisory). */
int awl_bufferqueue_count(struct awl_bufferqueue* q);     /* elements incl. head */
int awl_bufferqueue_pending(struct awl_bufferqueue* q);   /* elements behind the head */

/* ---- sync helpers (Linux dma-buf / sync_file ioctls; no Android) ----
 * fence_signaled: 1 = signaled (or fd < 0), 0 = pending, <0 = error.
 * fence_wait:     0 = signaled, 1 = timeout, <0 = error.
 * dmabuf_export_sync_file: sync_file of the dma-buf's current write fences
 *   (what a reader must wait for); -1 = unsupported/failed (caller falls back
 *   to polling the dma-buf itself).
 * dmabuf_import_sync_file: add a fence to the dma-buf's reservation (read
 *   usage when read=1): an implicit-sync writer then waits for it.
 * fence_merge: sync_file merge of a and b (either may be -1); returns a new
 *   fd (caller owns) or -1. Inputs are NOT closed. */
int awl_fence_signaled(int fd);
int awl_fence_wait(int fd, int timeout_ms);
int awl_dmabuf_export_sync_file(int dmabuf_fd);
int awl_dmabuf_import_sync_file(int dmabuf_fd, int fence_fd, int read);
int awl_fence_merge(int a, int b);
int awl_fence_is_sync_file(int fd);   /* 1 = a sync_file (SYNC_IOC_FILE_INFO answers) */

#ifdef __cplusplus
}
#endif
#endif
