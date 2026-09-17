/* awl_bufferqueue.c — bounded ring of dma-buf frames with lock-free push,
 * locked pop and per-element references (see awl_bufferqueue.h).
 *
 * Ring: head/tail are monotonically increasing counters (slot = idx % CAP)
 * holding element pointers. Producers reserve a slot with a CAS on tail,
 * store the element, then publish it with a release-store to the slot's
 * `ready` flag. The consumer (under the head lock) only sees consecutive
 * ready slots from head; it clears `ready` before advancing head
 * (release-store), so a producer that observes the new head (acquire) is
 * the only writer of that slot again. The only shared words are the two
 * counters and the per-slot flag.
 *
 * Element life: refs = 1 (ring) at push, +1 per gethead. The ring's ref goes
 * at pop (drain/flush), a consumer's at put. Last ref → release callback →
 * close fds → free. Every element also pins the queue object, so a queue is
 * never freed while a consumer still holds one of its frames.
 *
 * Readiness = the acquire fence signaled (poll POLLIN with zero timeout), or
 * for elements without an explicit fence the dma-buf's own write fences
 * (dma_buf_poll POLLIN waits for DMA_RESV_USAGE_WRITE only — exactly "the
 * writer is done", our own later read fences do not count). A NULL-buffer
 * marker is always ready. */
#include "awl_bufferqueue.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define AWL_TAG "anland-bq"
#include "awl_log.h"

#define AWL_BQ_CAP 8u   /* head + 7 pending: more buffers than any client
                         * allocates, so a detached (never drained) window
                         * starves the client of buffers = natural throttle;
                         * a bigger client overflows into push-fail (=
                         * immediate release, frame dropped) instead of
                         * deadlocking */

struct awl_bq_elem {
    struct awl_bq_buffer pub;      /* first: gethead hands out &pub */
    atomic_int refs;
    atomic_int rel_fd;             /* merged consumer fences (CAS merge, put) */
    struct awl_bufferqueue* q;     /* +1 ref while the element lives */
};

struct awl_bq_slot {
    struct awl_bq_elem* e;
    atomic_int ready;
};

struct awl_bufferqueue {
    atomic_int refs;
    awl_bq_release_fn release;
    void* ctx;
    pthread_mutex_t lock;          /* head lock */
    atomic_uint head;              /* consumer-owned (advanced under lock) */
    atomic_uint tail;              /* producer CAS */
    struct awl_bq_slot slots[AWL_BQ_CAP];
    atomic_int timeouts;           /* gethead fence timeouts (rate-limited log) */
    atomic_int armed;              /* a fence watch is registered (awl_bufferqueue_arm) */
    atomic_uint superseded;        /* frames drain popped unshown, monotonic (consumer pacing hint) */
};

/* ---------------- sync helpers ---------------- */

int awl_fence_signaled(int fd) {
    if (fd < 0) return 1;
    struct pollfd p = { fd, POLLIN, 0 };
    int r = poll(&p, 1, 0);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (r == 0) return 0;
    if (p.revents & POLLNVAL) return 1;   /* dead fd: never stall on it */
    return (p.revents & (POLLIN | POLLERR | POLLHUP)) ? 1 : 0;
}

int awl_fence_wait(int fd, int timeout_ms) {
    if (fd < 0) return 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        int r = poll(&p, 1, timeout_ms);
        if (r > 0) return 0;   /* POLLIN / POLLNVAL / POLLERR all end the wait */
        if (r == 0) return 1;
        if (errno != EINTR) return -1;
        if (timeout_ms >= 0) {   /* EINTR: keep the deadline */
            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long el = (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
            timeout_ms = el >= timeout_ms ? 0 : timeout_ms - (int)el;
        }
    }
}

int awl_dmabuf_export_sync_file(int dmabuf_fd) {
    static atomic_int unsupported;   /* ENOTTY once → stop trying, log once */
    if (dmabuf_fd < 0 || atomic_load(&unsupported)) return -1;
    struct dma_buf_export_sync_file arg = { .flags = DMA_BUF_SYNC_READ, .fd = -1 };
    if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &arg) != 0) {
        if (errno == ENOTTY || errno == EINVAL) {
            if (!atomic_exchange(&unsupported, 1))
                LOGE("DMA_BUF_IOCTL_EXPORT_SYNC_FILE unsupported (%s) — "
                     "readiness falls back to polling the dma-buf", strerror(errno));
        }
        return -1;
    }
    return arg.fd;
}

int awl_dmabuf_import_sync_file(int dmabuf_fd, int fence_fd, int read) {
    if (dmabuf_fd < 0 || fence_fd < 0) return -1;
    struct dma_buf_import_sync_file arg = {
        .flags = read ? DMA_BUF_SYNC_READ : DMA_BUF_SYNC_WRITE, .fd = fence_fd };
    return ioctl(dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &arg) == 0 ? 0 : -1;
}

int awl_fence_merge(int a, int b) {
    if (a < 0 && b < 0) return -1;
    if (a < 0) return fcntl(b, F_DUPFD_CLOEXEC, 0);
    if (b < 0) return fcntl(a, F_DUPFD_CLOEXEC, 0);
    struct sync_merge_data m;
    memset(&m, 0, sizeof(m));
    strncpy(m.name, "awl-release", sizeof(m.name) - 1);
    m.fd2 = b;
    m.fence = -1;
    if (ioctl(a, SYNC_IOC_MERGE, &m) != 0) return -1;
    return m.fence;
}

int awl_fence_is_sync_file(int fd) {
    if (fd < 0) return 0;
    struct sync_file_info info;
    memset(&info, 0, sizeof(info));
    return ioctl(fd, SYNC_IOC_FILE_INFO, &info) == 0;
}

/* ---------------- elements ---------------- */

static inline int elem_ready(const struct awl_bq_buffer* e) {
    if (e->dmabuf_fd < 0) return 1;                       /* NULL marker */
    if (e->acquire_fd >= 0) return awl_fence_signaled(e->acquire_fd) != 0;
    return awl_fence_signaled(e->dmabuf_fd) != 0;        /* implicit: writer fences */
}

static inline int elem_wait(const struct awl_bq_buffer* e, int timeout_ms) {
    if (e->dmabuf_fd < 0) return 0;
    return awl_fence_wait(e->acquire_fd >= 0 ? e->acquire_fd : e->dmabuf_fd, timeout_ms);
}

static void close_fd(int* fd) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

/* Drop one reference; the last one hands the frame back and frees. */
static void elem_unref(struct awl_bq_elem* e) {
    if (atomic_fetch_sub_explicit(&e->refs, 1, memory_order_acq_rel) != 1) return;
    struct awl_bufferqueue* q = e->q;
    e->pub.release_fd = atomic_load(&e->rel_fd);
    if (q->release) q->release(&e->pub, q->ctx);
    close_fd(&e->pub.dmabuf_fd);
    close_fd(&e->pub.acquire_fd);
    close_fd(&e->pub.release_fd);
    free(e);
    awl_bufferqueue_unref(q);
}

/* ---------------- fence waiter ----------------
 * One epoll thread for every queue: an armed queue contributes the fd of the
 * first incomplete frame behind its head (own dup; EPOLLONESHOT, explicitly
 * DEL'd before close — dma-buf/sync_file fds are shared open file
 * descriptions with the client, a plain close would leave the entry live).
 * On signal: drain (releases the superseded head), re-arm if a further
 * frame is still incomplete. */
struct bq_arm {
    int fd;
    struct awl_bufferqueue* q;    /* +1 ref while registered */
};

static struct {
    pthread_mutex_t lock;
    int epfd;
    int started;                  /* 0 = not yet, 1 = running, -1 = failed (don't retry) */
    pthread_t th;
} g_waiter = { PTHREAD_MUTEX_INITIALIZER, -1, 0, 0 };

static void* waiter_thread(void* arg) {
    (void)arg;
    for (;;) {
        struct epoll_event ev[16];
        int n = epoll_wait(g_waiter.epfd, ev, 16, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOGE("fence waiter: epoll_wait %s — waiter exiting", strerror(errno));
            return NULL;
        }
        for (int i = 0; i < n; i++) {
            struct bq_arm* a = ev[i].data.ptr;
            epoll_ctl(g_waiter.epfd, EPOLL_CTL_DEL, a->fd, NULL);
            close(a->fd);
            struct awl_bufferqueue* q = a->q;
            free(a);
            atomic_store(&q->armed, 0);
            awl_bufferqueue_lock(q);      /* consumers hold it for microseconds */
            awl_bufferqueue_drain(q);
            awl_bufferqueue_arm(q);
            awl_bufferqueue_unlock(q);
            awl_bufferqueue_unref(q);
        }
    }
}

static int waiter_start(void) {
    pthread_mutex_lock(&g_waiter.lock);
    if (!g_waiter.started) {
        g_waiter.epfd = epoll_create1(EPOLL_CLOEXEC);
        if (g_waiter.epfd >= 0 &&
            pthread_create(&g_waiter.th, NULL, waiter_thread, NULL) == 0) {
            g_waiter.started = 1;
        } else {
            LOGE("fence waiter: start failed (%s) — releases fall back to commit/vsync drains",
                 strerror(errno));
            if (g_waiter.epfd >= 0) close(g_waiter.epfd);
            g_waiter.epfd = -1;
            g_waiter.started = -1;
        }
    }
    int ok = g_waiter.started == 1;
    pthread_mutex_unlock(&g_waiter.lock);
    return ok;
}

/* ---------------- ring ---------------- */

/* Consecutive published elements from head (consumer side, under lock). */
static unsigned visible(struct awl_bufferqueue* q) {
    unsigned h = atomic_load_explicit(&q->head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&q->tail, memory_order_acquire);
    unsigned n = 0;
    while (n < t - h &&
           atomic_load_explicit(&q->slots[(h + n) % AWL_BQ_CAP].ready, memory_order_acquire))
        n++;
    return n;
}

static inline struct awl_bq_elem* at(struct awl_bufferqueue* q, unsigned idx) {
    return q->slots[idx % AWL_BQ_CAP].e;
}

/* Under lock, visible() >= 1: pop the head (drops the ring's reference). */
static void pop_head(struct awl_bufferqueue* q) {
    unsigned h = atomic_load_explicit(&q->head, memory_order_relaxed);
    struct awl_bq_slot* s = &q->slots[h % AWL_BQ_CAP];
    struct awl_bq_elem* e = s->e;
    s->e = NULL;
    atomic_store_explicit(&s->ready, 0, memory_order_relaxed);
    atomic_store_explicit(&q->head, h + 1, memory_order_release);   /* slot reusable by producers */
    elem_unref(e);
}

struct awl_bufferqueue* awl_bufferqueue_create(awl_bq_release_fn fn, void* ctx) {
    struct awl_bufferqueue* q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    atomic_init(&q->refs, 1);
    q->release = fn;
    q->ctx = ctx;
    pthread_mutex_init(&q->lock, NULL);
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
    atomic_init(&q->armed, 0);
    atomic_init(&q->superseded, 0);
    for (unsigned i = 0; i < AWL_BQ_CAP; i++) atomic_init(&q->slots[i].ready, 0);
    return q;
}

void awl_bufferqueue_ref(struct awl_bufferqueue* q) {
    atomic_fetch_add(&q->refs, 1);
}

void awl_bufferqueue_unref(struct awl_bufferqueue* q) {
    if (!q || atomic_fetch_sub_explicit(&q->refs, 1, memory_order_acq_rel) != 1) return;
    /* every live element pins the queue → at zero the ring holds nothing
     * published; a slot reserved by a producer that never published cannot
     * exist either (producers hold the owner's reference) */
    if (atomic_load(&q->head) != atomic_load(&q->tail))
        LOGE("bufferqueue freed with %u slots reserved", atomic_load(&q->tail) - atomic_load(&q->head));
    pthread_mutex_destroy(&q->lock);
    free(q);
}

int awl_bufferqueue_push(struct awl_bufferqueue* q, const struct awl_bq_buffer* e) {
    static atomic_uint_fast64_t seq;   /* never 0: consumers use 0 as "none" */
    struct awl_bq_elem* el = malloc(sizeof(*el));
    if (!el) return 0;
    el->pub = *e;
    el->pub.release_fd = -1;
    el->pub.seq = atomic_fetch_add(&seq, 1) + 1;
    atomic_init(&el->refs, 1);
    atomic_init(&el->rel_fd, e->release_fd);   /* a producer-supplied fence is merged like a consumer's */
    el->q = q;
    unsigned t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    for (;;) {
        unsigned h = atomic_load_explicit(&q->head, memory_order_acquire);
        if (t - h >= AWL_BQ_CAP) {   /* full */
            free(el);
            return 0;
        }
        if (atomic_compare_exchange_weak_explicit(&q->tail, &t, t + 1,
                                                  memory_order_acq_rel,
                                                  memory_order_relaxed))
            break;
        /* t reloaded by the failed CAS */
    }
    awl_bufferqueue_ref(q);   /* the element pins the queue */
    struct awl_bq_slot* s = &q->slots[t % AWL_BQ_CAP];
    s->e = el;
    atomic_store_explicit(&s->ready, 1, memory_order_release);
    return 1;
}

int awl_bufferqueue_trylock(struct awl_bufferqueue* q) {
    return pthread_mutex_trylock(&q->lock) == 0;
}
void awl_bufferqueue_lock(struct awl_bufferqueue* q) {
    pthread_mutex_lock(&q->lock);
}
void awl_bufferqueue_unlock(struct awl_bufferqueue* q) {
    pthread_mutex_unlock(&q->lock);
}

int awl_bufferqueue_drain(struct awl_bufferqueue* q) {
    int n = 0;
    while (visible(q) >= 2) {
        unsigned h = atomic_load_explicit(&q->head, memory_order_relaxed);
        if (!elem_ready(&at(q, h + 1)->pub)) break;   /* FIFO completion: nothing behind it is ready either */
        pop_head(q);
        n++;
    }
    if (n) atomic_fetch_add_explicit(&q->superseded, (unsigned)n, memory_order_relaxed);
    return n;
}

unsigned awl_bufferqueue_superseded(struct awl_bufferqueue* q) {
    return atomic_load_explicit(&q->superseded, memory_order_relaxed);
}

struct awl_bq_buffer* awl_bufferqueue_gethead(struct awl_bufferqueue* q, int timeout_ms) {
    if (visible(q) == 0) return NULL;
    unsigned h = atomic_load_explicit(&q->head, memory_order_relaxed);
    struct awl_bq_elem* e = at(q, h);
    if (!elem_ready(&e->pub)) {
        int r = elem_wait(&e->pub, timeout_ms);
        if (r == 1) {
            int n = atomic_fetch_add(&q->timeouts, 1) + 1;
            if (n <= 3 || (n % 100) == 0)
                LOGE("gethead: acquire fence not signaled in %dms (%ux%u ino=%llu, %s fence) — waiting for completion (#%d)",
                     timeout_ms, e->pub.width, e->pub.height, (unsigned long long)e->pub.ino,
                     e->pub.acquire_fd >= 0 ? "explicit" : "implicit", n);
            /* the contract is a COMPLETE head: after the warning threshold,
             * block on the fence until the writer is done. (A dead fd ends
             * the poll immediately; only a real wait error gives up —
             * presenting an incomplete buffer would show garbage.) */
            if (elem_wait(&e->pub, -1) < 0)
                LOGE("gethead: fence wait failed (%s) — returning the head anyway",
                     strerror(errno));
        }
    }
    atomic_fetch_add_explicit(&e->refs, 1, memory_order_relaxed);   /* consumer's reference */
    return &e->pub;
}

struct awl_bq_buffer* awl_bufferqueue_tryhead(struct awl_bufferqueue* q) {
    if (visible(q) == 0) return NULL;
    unsigned h = atomic_load_explicit(&q->head, memory_order_relaxed);
    struct awl_bq_elem* e = at(q, h);
    if (!elem_ready(&e->pub)) return NULL;   /* writer still busy: nothing to present yet */
    atomic_fetch_add_explicit(&e->refs, 1, memory_order_relaxed);   /* consumer's reference */
    return &e->pub;
}

void awl_bufferqueue_put(struct awl_bq_buffer* pub, int fence_fd) {
    if (!pub) return;
    struct awl_bq_elem* e = (struct awl_bq_elem*)pub;   /* pub is the first member */
    if (fence_fd >= 0 && pub->dmabuf_fd >= 0) {
        /* lock-free merge: several consumers (cursor image crossing windows)
         * may put the same element — CAS the merged fd in, retry on a race */
        for (;;) {
            int old = atomic_load(&e->rel_fd);
            int merged = awl_fence_merge(old, fence_fd);   /* dup when none yet */
            if (merged < 0) break;                          /* keep what is there */
            if (atomic_compare_exchange_strong(&e->rel_fd, &old, merged)) {
                if (old >= 0) close(old);
                break;
            }
            close(merged);
        }
    }
    elem_unref(e);
}

void awl_bufferqueue_arm(struct awl_bufferqueue* q) {
    if (visible(q) < 2) return;
    unsigned h = atomic_load_explicit(&q->head, memory_order_relaxed);
    const struct awl_bq_buffer* next = &at(q, h + 1)->pub;
    if (elem_ready(next)) return;   /* drain will take it at the next opportunity */
    if (atomic_exchange(&q->armed, 1)) return;
    if (!waiter_start()) { atomic_store(&q->armed, 0); return; }
    int src = next->acquire_fd >= 0 ? next->acquire_fd : next->dmabuf_fd;
    struct bq_arm* a = malloc(sizeof(*a));
    int fd = a ? fcntl(src, F_DUPFD_CLOEXEC, 0) : -1;
    if (fd < 0) {
        free(a);
        atomic_store(&q->armed, 0);
        return;
    }
    a->fd = fd;
    a->q = q;
    awl_bufferqueue_ref(q);
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLONESHOT;
    ev.data.ptr = a;
    if (epoll_ctl(g_waiter.epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        LOGE("fence waiter: epoll add %s", strerror(errno));
        close(fd);
        free(a);
        awl_bufferqueue_unref(q);
        atomic_store(&q->armed, 0);
    }
}

void awl_bufferqueue_flush(struct awl_bufferqueue* q) {
    while (visible(q) > 0) pop_head(q);
}

int awl_bufferqueue_count(struct awl_bufferqueue* q) {
    unsigned h = atomic_load_explicit(&q->head, memory_order_acquire);
    unsigned t = atomic_load_explicit(&q->tail, memory_order_acquire);
    return (int)(t - h);
}

int awl_bufferqueue_pending(struct awl_bufferqueue* q) {
    int n = awl_bufferqueue_count(q);
    return n > 1 ? n - 1 : 0;
}
