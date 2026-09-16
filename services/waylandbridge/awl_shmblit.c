/* awl_shmblit.c — shm → dmabuf converter ("internal commits")
 *
 * A wl_shm client hands us CPU memory; the frame stream to the renderer is
 * dma-buf only (awl_bufferqueue). Per shm surface this module keeps three
 * CPU-writable dma-bufs (kernel DMA heap, awl_dmaheap.c) and, on a global
 * blit thread ticking at the output refresh rate, copies the damaged region
 * of the client's current shm buffer into a free one and pushes it into the
 * surface's queue like any dmabuf commit. Between ticks damage accumulates
 * (the client may commit as fast as it likes — one copy per vsync at most).
 *
 * Damage bookkeeping (classic buffer-age): every consumed damage rect is
 * OR'd into all three slots; the slot that gets written copies its own
 * accumulated bbox (everything that changed since it was last written) and
 * clears it. A geometry change reallocates the written slot and forces a
 * full copy.
 *
 * Slot life: FREE → (copy) READY → (push) QUEUED → (queue release callback)
 * FREE. A push into a full queue keeps the slot READY and retries next tick.
 *
 * wl_buffer.release for the shm buffer goes out right after the first copy
 * from it (the pixels are ours now; a buffer never copied is released when
 * it is replaced) — earlier than the old present-time release, and never
 * while a copy is reading it. In-place redraw clients (damage+commit, no
 * attach) keep the same buffer current and are re-copied per tick.
 *
 * Locks: g_blit.lock = thread scheduling + slot states (also taken by the
 * queue release callback on any thread); the surface's ev_lock = protocol
 * side (cur/retired shm refs, damage). Order: ev_lock → g_blit.lock (commit
 * path signals work under ev_lock); the blit thread never nests them. The
 * blit thread additionally takes rwl.rd (schedule_render) — only with
 * neither of the above held. */
#define AWL_TAG "anland-shm"   /* before awl_internal.h → awl_log.h picks it up */
#include "awl_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SLOTS 3

enum { SLOT_FREE = 0, SLOT_READY, SLOT_QUEUED };

struct shm_slot {
    int fd;                 /* dma-buf (-1 = unallocated) */
    void* map;
    size_t size;
    uint32_t w, h, stride, format;
    uint64_t ino;
    int state;
    int dirty_full, dirty_any;
    int32_t dx, dy, dw, dh; /* accumulated damage bbox (buffer px) */
};

struct shm_ref {
    struct wl_resource* res;          /* wl_buffer (shm) */
    struct wl_resource* release_res;  /* zwp_linux_buffer_release_v1 of its commit (may be NULL) */
};

struct awl_shmblit {
    atomic_int refs;
    struct awl_surface* s;            /* g_blit.lock; NULL once detached */
    struct wl_list link;              /* g_blit.list */

    /* ---- owned by s->ev_lock ---- */
    struct shm_ref cur;               /* buffer we copy from (NULL = none) */
    uint32_t gen;                     /* bumps per attach: pairs post-copy release with the right cur */
    int cur_released;                 /* wl_buffer.release already sent for cur */
    struct shm_ref retired[4];        /* replaced while being copied: release after the copy */
    int n_retired;
    struct wl_resource* copying;      /* res currently pinned by the blit thread */
    uint32_t copying_gen;

    /* ---- owned by g_blit.lock ---- */
    int work;                         /* damage to copy / READY slot to push */
    int busy;                         /* blit thread inside process() */
    int dead;                         /* detached: thread must not touch s */
    struct shm_slot slot[SLOTS];
};

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    pthread_t th;
    int started, stop;
    struct wl_list list;
    int64_t next_tick_ns;
} g_blit = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, { NULL, NULL }, 0 };

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static uint32_t shm_fourcc(uint32_t wl_fmt) {
    return wl_fmt == WL_SHM_FORMAT_XRGB8888 ? AWL_FORMAT_XRGB8888 : AWL_FORMAT_ARGB8888;
}

/* ---------------- slots ---------------- */

static void slot_free_mem(struct shm_slot* sl) {
    if (sl->map) awl_dmabuf_unmap(sl->map, sl->size);
    if (sl->fd >= 0) close(sl->fd);
    sl->map = NULL;
    sl->fd = -1;
    sl->size = 0;
    sl->w = sl->h = sl->stride = 0;
    sl->ino = 0;
}

/* Linear BGRA: row pitch padded to 64 px (256 B) — a superset of every
 * alignment the GPU could want for a linear sampled texture, and the value
 * we both write with and declare (the renderer's AHB forge patches exactly
 * this stride into the metadata blob; the GPU samples by it — verified). */
static int slot_alloc(struct shm_slot* sl, uint32_t w, uint32_t h, uint32_t format) {
    slot_free_mem(sl);
    uint32_t stride = ((w + 63u) & ~63u) * 4u;
    size_t size = ((size_t)stride * h + 4095u) & ~(size_t)4095u;
    int fd = awl_dmaheap_alloc(size);
    if (fd < 0) return -1;
    void* map = awl_dmabuf_map(fd, size);
    if (!map) {
        close(fd);
        return -1;
    }
    struct stat st;
    sl->ino = fstat(fd, &st) == 0 ? (uint64_t)st.st_ino : 0;
    sl->fd = fd;
    sl->map = map;
    sl->size = size;
    sl->w = w;
    sl->h = h;
    sl->stride = stride;
    sl->format = format;
    sl->dirty_full = 1;   /* fresh memory: everything must be written */
    sl->dirty_any = 1;
    return 0;
}

static void slot_add_damage(struct shm_slot* sl, int st, int32_t x, int32_t y,
                            int32_t w, int32_t h) {
    if (st == AWL_DMG_FULL) {
        sl->dirty_full = 1;
        sl->dirty_any = 1;
        return;
    }
    if (st != AWL_DMG_RECT || w <= 0 || h <= 0) return;
    if (!sl->dirty_any) {
        sl->dx = x; sl->dy = y; sl->dw = w; sl->dh = h;
        sl->dirty_any = 1;
        return;
    }
    if (sl->dirty_full) return;
    int32_t x2 = sl->dx + sl->dw, y2 = sl->dy + sl->dh;
    if (x < sl->dx) sl->dx = x;
    if (y < sl->dy) sl->dy = y;
    if (x + w > x2) x2 = x + w;
    if (y + h > y2) y2 = y + h;
    sl->dw = x2 - sl->dx;
    sl->dh = y2 - sl->dy;
}

/* ---------------- refcount / lifecycle ---------------- */

static void* blit_thread(void* arg);

struct awl_shmblit* awl_shmblit_create(struct awl_surface* s) {
    struct awl_shmblit* sb = calloc(1, sizeof(*sb));
    if (!sb) return NULL;
    atomic_init(&sb->refs, 1);
    sb->s = s;
    for (int i = 0; i < SLOTS; i++) sb->slot[i].fd = -1;
    pthread_mutex_lock(&g_blit.lock);
    if (!g_blit.list.next) wl_list_init(&g_blit.list);
    if (!g_blit.started) {
        g_blit.stop = 0;
        if (pthread_create(&g_blit.th, NULL, blit_thread, NULL) == 0) g_blit.started = 1;
        else LOGE("shm blit thread: %s", strerror(errno));
    }
    wl_list_insert(g_blit.list.prev, &sb->link);
    pthread_mutex_unlock(&g_blit.lock);
    LOGI("surface %llu: shm converter created", (unsigned long long)s->id);
    return sb;
}

void awl_shmblit_ref(struct awl_shmblit* sb) {
    atomic_fetch_add(&sb->refs, 1);
}

void awl_shmblit_unref(struct awl_shmblit* sb) {
    if (!sb || atomic_fetch_sub(&sb->refs, 1) != 1) return;
    /* last reference: detached already (the surface's ref was dropped after
     * detach) and no frame references a slot any more */
    for (int i = 0; i < SLOTS; i++) slot_free_mem(&sb->slot[i]);
    free(sb);
}

void awl_shmblit_detach(struct awl_shmblit* sb) {
    pthread_mutex_lock(&g_blit.lock);
    sb->dead = 1;
    while (sb->busy) pthread_cond_wait(&g_blit.cv, &g_blit.lock);
    if (sb->link.next) {
        wl_list_remove(&sb->link);
        sb->link.next = sb->link.prev = NULL;
    }
    sb->s = NULL;
    pthread_mutex_unlock(&g_blit.lock);
}

void awl_shmblit_shutdown(void) {
    pthread_mutex_lock(&g_blit.lock);
    int started = g_blit.started;
    g_blit.stop = 1;
    g_blit.started = 0;
    pthread_cond_broadcast(&g_blit.cv);
    pthread_mutex_unlock(&g_blit.lock);
    if (started) pthread_join(g_blit.th, NULL);
}

/* ---------------- protocol side (ev_lock held) ---------------- */

static void kick(struct awl_shmblit* sb) {
    pthread_mutex_lock(&g_blit.lock);
    if (!sb->dead) {
        sb->work = 1;
        pthread_cond_broadcast(&g_blit.cv);
    }
    pthread_mutex_unlock(&g_blit.lock);
}

/* ev_lock held: hand a replaced/dying shm ref back to the client */
static void ref_release_locked(struct shm_ref* r) {
    if (!r->res) return;
    wl_buffer_send_release(r->res);
    if (r->release_res) {
        pthread_mutex_lock(&g_bufref_lock);
        awl_esync_release_locked(r->release_res, -1);
        pthread_mutex_unlock(&g_bufref_lock);
    }
    wl_client_flush(wl_resource_get_client(r->res));
    r->res = NULL;
    r->release_res = NULL;
}

void awl_shmblit_attach_locked(struct awl_shmblit* sb, struct wl_resource* res,
                               struct wl_resource* release_res) {
    struct shm_ref old = sb->cur;
    int old_released = sb->cur_released;
    sb->cur.res = res;
    sb->cur.release_res = release_res;
    sb->cur_released = 0;
    sb->gen++;
    if (old.res && old.res != res) {
        if (old.res == sb->copying) {   /* mid-copy: release after the copy */
            if (sb->n_retired < 4) sb->retired[sb->n_retired++] = old;
            else ref_release_locked(&old);   /* pathological churn: fall back to now */
        } else if (!old_released) {
            ref_release_locked(&old);
        } else if (old.release_res) {   /* wl_buffer.release already went out, the release object did not */
            struct shm_ref r = { NULL, old.release_res };
            pthread_mutex_lock(&g_bufref_lock);
            awl_esync_release_locked(r.release_res, -1);
            pthread_mutex_unlock(&g_bufref_lock);
        }
    } else if (old.res == res && res) {
        /* re-attach of the same buffer: one release per attach cycle */
        sb->cur_released = 0;
    }
    if (res) {
        kick(sb);
    } else {
        /* detach: a converted-but-unpushed frame must not surface after the
         * NULL marker the caller pushes — drop it (its slot is free again) */
        pthread_mutex_lock(&g_blit.lock);
        for (int i = 0; i < SLOTS; i++)
            if (sb->slot[i].state == SLOT_READY) sb->slot[i].state = SLOT_FREE;
        pthread_mutex_unlock(&g_blit.lock);
    }
}

void awl_shmblit_damaged_locked(struct awl_shmblit* sb) {
    if (sb->cur.res) kick(sb);
}

void awl_shmblit_release_gone_locked(struct awl_shmblit* sb, struct wl_resource* release_res) {
    if (sb->cur.release_res == release_res) sb->cur.release_res = NULL;
    for (int i = 0; i < sb->n_retired; i++)
        if (sb->retired[i].release_res == release_res) sb->retired[i].release_res = NULL;
}

void awl_shmblit_buffer_gone_locked(struct awl_shmblit* sb, struct wl_resource* res) {
    if (sb->cur.res == res) { sb->cur.res = NULL; sb->cur.release_res = NULL; }
    if (sb->copying == res) sb->copying = NULL;   /* pinned wl_shm_buffer keeps the memory; no release to send */
    for (int i = 0; i < sb->n_retired; ) {
        if (sb->retired[i].res == res) sb->retired[i] = sb->retired[--sb->n_retired];
        else i++;
    }
}

/* Queue release callback (any thread): the converted frame left the queue */
void awl_shmblit_slot_released(struct awl_shmblit* sb, int slot) {
    pthread_mutex_lock(&g_blit.lock);
    if (slot >= 0 && slot < SLOTS) sb->slot[slot].state = SLOT_FREE;
    if (sb->work && !sb->dead) pthread_cond_broadcast(&g_blit.cv);   /* a copy was waiting for a slot */
    pthread_mutex_unlock(&g_blit.lock);
}

/* ---------------- blit thread ---------------- */

/* Copy `rows` of the shm buffer into the slot (both BGRA, 4 B/px) */
static void copy_rect(struct shm_slot* sl, const uint8_t* src, int32_t sstride,
                      int32_t x, int32_t y, int32_t w, int32_t h) {
    uint8_t* dst = sl->map;
    if (x == 0 && w == (int32_t)sl->w && sstride == (int32_t)sl->stride) {
        memcpy(dst + (size_t)y * sl->stride, src + (size_t)y * sstride,
               (size_t)sl->stride * (size_t)h);   /* same pitch: one memcpy */
        return;
    }
    for (int32_t r = 0; r < h; r++)
        memcpy(dst + (size_t)(y + r) * sl->stride + (size_t)x * 4,
               src + (size_t)(y + r) * sstride + (size_t)x * 4,
               (size_t)w * 4);
}

static void push_ready(struct awl_shmblit* sb, struct awl_surface* s) {
    int pushed = 0;
    pthread_mutex_lock(&g_blit.lock);
    for (int i = 0; i < SLOTS; i++) {
        struct shm_slot* sl = &sb->slot[i];
        if (sl->state != SLOT_READY) continue;
        struct awl_bq_buffer e;
        memset(&e, 0, sizeof(e));
        e.dmabuf_fd = fcntl(sl->fd, F_DUPFD_CLOEXEC, 0);
        if (e.dmabuf_fd < 0) continue;
        e.acquire_fd = -1;      /* CPU-written + cache-flushed: no fence pending */
        e.release_fd = -1;
        e.ino = sl->ino;
        e.width = sl->w;
        e.height = sl->h;
        e.stride = sl->stride;
        e.format = sl->format;
        e.modifier = DRM_FORMAT_MOD_LINEAR;
        awl_shmblit_ref(sb);    /* the frame's cookie keeps the converter alive */
        e.user = awl_bufref_shm(sb, i);
        if (!e.user) { awl_shmblit_unref(sb); close(e.dmabuf_fd); continue; }
        if (awl_bufferqueue_push(s->q, &e)) {
            sl->state = SLOT_QUEUED;
            pushed = 1;
        } else {                /* queue full (renderer stalled): retry next tick */
            free(e.user);
            awl_shmblit_unref(sb);
            close(e.dmabuf_fd);
            sb->work = 1;
        }
    }
    pthread_mutex_unlock(&g_blit.lock);
    if (pushed) {
        awl_surface_commit_drain(s);      /* mailbox: drop frames the renderer never got to */
        awl_surface_schedule_render(s);
    }
}

/* One tick of work for a surface. Called with g_blit.lock NOT held,
 * sb->busy = 1 (detach waits on it), so s stays valid throughout. */
static void process(struct awl_shmblit* sb, struct awl_surface* s) {
    /* 1. protocol snapshot: damage since last time + pin the source */
    pthread_mutex_lock(&s->ev_lock);
    int st = s->cd_state;
    int32_t bs = s->buf_scale > 1 ? s->buf_scale : 1;
    int32_t dx = s->cur_damage_x * bs, dy = s->cur_damage_y * bs;
    int32_t dw = s->cur_damage_w * bs, dh = s->cur_damage_h * bs;
    s->cd_state = AWL_DMG_NONE;
    struct wl_resource* res = sb->cur.res;
    struct wl_shm_buffer* shm = res ? wl_shm_buffer_get(res) : NULL;
    struct wl_shm_pool* pool = NULL;
    uint32_t w = 0, h = 0, fmt = 0;
    int32_t sstride = 0;
    uint32_t gen = sb->gen;
    if (shm) {
        wl_shm_buffer_ref(shm);
        pool = wl_shm_buffer_ref_pool(shm);
        w = (uint32_t)wl_shm_buffer_get_width(shm);
        h = (uint32_t)wl_shm_buffer_get_height(shm);
        sstride = wl_shm_buffer_get_stride(shm);
        fmt = shm_fourcc(wl_shm_buffer_get_format(shm));
        sb->copying = res;
        sb->copying_gen = gen;
    }
    pthread_mutex_unlock(&s->ev_lock);

    /* 2. slot bookkeeping */
    struct shm_slot* sl = NULL;
    int idx = -1;
    int32_t cx = 0, cy = 0, cw = 0, ch = 0;
    int need_copy = 0;
    pthread_mutex_lock(&g_blit.lock);
    sb->work = 0;
    for (int i = 0; i < SLOTS; i++) slot_add_damage(&sb->slot[i], st, dx, dy, dw, dh);
    if (shm && w && h) {
        for (int i = 0; i < SLOTS && !sl; i++)
            if (sb->slot[i].state == SLOT_FREE) { sl = &sb->slot[i]; idx = i; }
        if (!sl) {
            sb->work = 1;   /* all three in flight: wait for a release */
        } else {
            if (sl->fd < 0 || sl->w != w || sl->h != h || sl->format != fmt) {
                if (slot_alloc(sl, w, h, fmt) != 0) {
                    sl = NULL;
                    sb->work = 1;   /* allocation failed: retry later (logged by the allocator) */
                }
            }
            if (sl && sl->dirty_any) {
                need_copy = 1;
                if (sl->dirty_full) { cx = 0; cy = 0; cw = (int32_t)w; ch = (int32_t)h; }
                else {
                    cx = sl->dx < 0 ? 0 : sl->dx;
                    cy = sl->dy < 0 ? 0 : sl->dy;
                    int32_t x2 = sl->dx + sl->dw, y2 = sl->dy + sl->dh;
                    if (x2 > (int32_t)w) x2 = (int32_t)w;
                    if (y2 > (int32_t)h) y2 = (int32_t)h;
                    cw = x2 - cx;
                    ch = y2 - cy;
                }
                sl->dirty_any = 0;
                sl->dirty_full = 0;
                sl->dx = sl->dy = sl->dw = sl->dh = 0;
            } else if (sl) {
                /* nothing changed since this slot was written — its content
                 * is current. Only re-present when something asked for it
                 * (an attach of the same content / in-place redraw with no
                 * damage: protocol FULL → handled above), so no push. */
                sl = NULL;
            }
        }
    }
    pthread_mutex_unlock(&g_blit.lock);

    /* 3. copy (no locks: the shm refs pin the mapping, the slot is FREE so
     *    nobody else touches it) */
    if (sl && need_copy && cw > 0 && ch > 0) {
        wl_shm_buffer_begin_access(shm);
        const uint8_t* src = wl_shm_buffer_get_data(shm);
        if (src) {
            awl_dmabuf_cpu_sync(sl->fd, 1, 1);
            copy_rect(sl, src, sstride, cx, cy, cw, ch);
            awl_dmabuf_cpu_sync(sl->fd, 0, 1);
        }
        wl_shm_buffer_end_access(shm);   /* a faulted pool kills the buffer resource (SIGBUS guard) */
        pthread_mutex_lock(&g_blit.lock);
        sl->state = src ? SLOT_READY : SLOT_FREE;
        pthread_mutex_unlock(&g_blit.lock);
    } else if (sl && !need_copy) {
        pthread_mutex_lock(&g_blit.lock);
        sl->state = SLOT_FREE;
        pthread_mutex_unlock(&g_blit.lock);
    }
    if (shm) {
        wl_shm_buffer_unref(shm);
        wl_shm_pool_unref(pool);
    }

    /* 4. release the source: first copy of this attach hands the buffer
     *    back (pixels are ours); retired buffers replaced mid-copy too */
    pthread_mutex_lock(&s->ev_lock);
    sb->copying = NULL;
    if (res && sb->cur.res == res && sb->gen == gen && !sb->cur_released) {
        sb->cur_released = 1;
        wl_buffer_send_release(res);
        if (sb->cur.release_res) {
            pthread_mutex_lock(&g_bufref_lock);
            awl_esync_release_locked(sb->cur.release_res, -1);
            pthread_mutex_unlock(&g_bufref_lock);
            sb->cur.release_res = NULL;
        }
        wl_client_flush(wl_resource_get_client(res));
    }
    for (int i = 0; i < sb->n_retired; i++) ref_release_locked(&sb->retired[i]);
    sb->n_retired = 0;
    pthread_mutex_unlock(&s->ev_lock);

    /* 5. internal commit of whatever is READY */
    push_ready(sb, s);
}

static void* blit_thread(void* arg) {
    (void)arg;
    pthread_mutex_lock(&g_blit.lock);
    for (;;) {
        if (g_blit.stop) break;
        /* anything to do? */
        struct awl_shmblit* sb;
        int any = 0;
        wl_list_for_each(sb, &g_blit.list, link)
            if (sb->work && !sb->dead) { any = 1; break; }
        if (!any) {
            pthread_cond_wait(&g_blit.cv, &g_blit.lock);
            continue;
        }
        /* pace at the output refresh rate: one pass per period, phase-free
         * (no Android Choreographer in the logic layer — the renderer's
         * eglSwapBuffers is what actually aligns to vsync) */
        int64_t period = 1000000000LL / (g_srv.info.refresh_hz > 0 ? g_srv.info.refresh_hz : 60);
        int64_t now = now_ns();
        if (now < g_blit.next_tick_ns) {
            struct timespec until = { (time_t)(g_blit.next_tick_ns / 1000000000LL),
                                      (long)(g_blit.next_tick_ns % 1000000000LL) };
            pthread_cond_timedwait(&g_blit.cv, &g_blit.lock, &until);
            continue;   /* re-evaluate (stop / work / time) */
        }
        g_blit.next_tick_ns = (g_blit.next_tick_ns + period > now) ? g_blit.next_tick_ns + period
                                                                    : now + period;
        /* snapshot the workers (the list may change while we run unlocked) */
        struct awl_shmblit* batch[64];
        int n = 0;
        wl_list_for_each(sb, &g_blit.list, link) {
            if (!sb->work || sb->dead || n >= 64) continue;
            sb->busy = 1;
            awl_shmblit_ref(sb);
            batch[n++] = sb;
        }
        pthread_mutex_unlock(&g_blit.lock);
        for (int i = 0; i < n; i++) {
            struct awl_surface* s = batch[i]->s;   /* stable: detach waits for busy=0 */
            if (s) process(batch[i], s);
        }
        pthread_mutex_lock(&g_blit.lock);
        for (int i = 0; i < n; i++) {
            batch[i]->busy = 0;
            awl_shmblit_unref(batch[i]);
        }
        pthread_cond_broadcast(&g_blit.cv);   /* wake a detach waiting on busy */
    }
    pthread_mutex_unlock(&g_blit.lock);
    return NULL;
}
