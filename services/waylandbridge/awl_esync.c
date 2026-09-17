/* awl_esync.c — zwp_linux_explicit_synchronization_v1 (v2)
 *
 * Per-surface explicit synchronization: the client hands us an acquire fence
 * (sync_file) with each commit and optionally asks for a release object that
 * receives a fence for our last read of that buffer. Both are double-buffered
 * surface state (applied on the commit that carries the attach; a
 * sync-subsurface latch moves them along with the buffer — awl_subsurface.c).
 *
 * Wiring:
 *   set_acquire_fence → s->pend_acquire_fd (owned) — surface_commit moves it
 *     into the queue element (awl_surface_apply_buffer): the renderer never
 *     samples before it signals (awl_bufferqueue readiness).
 *   get_release → s->pend_release_res — surface_commit binds it to the
 *     element's cookie (awl_bufref.release_res). When the element leaves the
 *     queue (bq_release_cb, any thread) fenced_release carries the merged
 *     render fence the GPU composite produced (or immediate_release when we
 *     never sampled it), then the resource is parked in s->esync_gc:
 *     wl_resource_destroy is not thread safe against the dispatch thread's
 *     object map, so the destroy runs there at the next request of that
 *     surface (awl_esync_gc) — the client already dropped its proxy, a
 *     parked id is inert.
 *
 * Validation at commit (awl_esync_commit_check): fence/release without an
 * attached buffer → no_buffer; acquire fence on a wl_shm buffer →
 * unsupported_buffer (the release object alone is fine on shm: the converter
 * sends immediate_release once it has copied the pixels). */
#include "awl_internal.h"
#include "linux-explicit-synchronization-unstable-v1-server-protocol.h"

#include <unistd.h>

struct awl_esync_release {
    struct wl_resource* res;
    struct awl_surface* s;        /* owner (dispatch thread; every release object
                                   * dies with its surface in awl_esync_surface_gone) */
    struct awl_bufref* ref;       /* queued frame carrying us (g_bufref_lock) */
    struct wl_list all_link;      /* s->esync_all */
    struct wl_list gc_link;       /* s->esync_gc once delivered (g_bufref_lock) */
    int delivered;
};

/* ---------------- release object ---------------- */

static void release_res_destroy(struct wl_resource* res) {
    struct awl_esync_release* er = wl_resource_get_user_data(res);
    if (!er) return;
    struct awl_surface* s = er->s;
    pthread_mutex_lock(&g_bufref_lock);
    if (er->ref) er->ref->release_res = NULL;
    if (er->delivered) wl_list_remove(&er->gc_link);
    pthread_mutex_unlock(&g_bufref_lock);
    if (s) {
        wl_list_remove(&er->all_link);
        if (s->pend_release_res == res) s->pend_release_res = NULL;
        if (s->latched_release_res == res) s->latched_release_res = NULL;
        /* a shm commit keeps it as the release object of its source buffer
         * until a backend has read the pixels (ev_lock-owned word) */
        awl_surface_shm_release_gone(s, res);
    }
    free(er);
}

void awl_esync_release_locked(struct wl_resource* res, int fence_fd) {
    struct awl_esync_release* er = wl_resource_get_user_data(res);
    if (!er || er->delivered) return;
    if (fence_fd >= 0)
        zwp_linux_buffer_release_v1_send_fenced_release(res, fence_fd);
    else
        zwp_linux_buffer_release_v1_send_immediate_release(res);
    er->delivered = 1;
    if (er->ref) {
        er->ref->release_res = NULL;
        er->ref = NULL;
    }
    wl_list_insert(er->s->esync_gc.prev, &er->gc_link);
}

void awl_esync_bind_ref(struct wl_resource* res, struct awl_bufref* ref) {
    struct awl_esync_release* er = wl_resource_get_user_data(res);
    if (!er) return;
    pthread_mutex_lock(&g_bufref_lock);
    er->ref = ref;
    pthread_mutex_unlock(&g_bufref_lock);
}

void awl_esync_gc(struct awl_surface* s) {
    struct wl_list batch;
    wl_list_init(&batch);
    pthread_mutex_lock(&g_bufref_lock);
    if (!wl_list_empty(&s->esync_gc)) {
        wl_list_insert_list(&batch, &s->esync_gc);
        wl_list_init(&s->esync_gc);
    }
    pthread_mutex_unlock(&g_bufref_lock);
    struct awl_esync_release* er;
    struct awl_esync_release* tmp;
    wl_list_for_each_safe(er, tmp, &batch, gc_link)
        wl_resource_destroy(er->res);   /* handler unlinks gc_link from `batch` + frees */
}

/* ---------------- surface synchronization object ---------------- */

static void sync_destroy(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static void sync_res_destroy(struct wl_resource* res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) return;
    /* fences set since the last commit are discarded; release objects are not affected */
    if (s->pend_acquire_fd >= 0) {
        close(s->pend_acquire_fd);
        s->pend_acquire_fd = -1;
    }
    if (s->sync_res == res) s->sync_res = NULL;
}

static void sync_set_acquire_fence(struct wl_client* c, struct wl_resource* res, int32_t fd) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) {
        close(fd);
        wl_resource_post_error(res, ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_NO_SURFACE,
                               "the wl_surface was destroyed");
        return;
    }
    awl_esync_gc(s);
    if (!awl_fence_is_sync_file(fd)) {
        close(fd);
        wl_resource_post_error(res, ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_INVALID_FENCE,
                               "fd is not a sync_file");
        return;
    }
    if (s->pend_acquire_fd >= 0) {
        close(fd);
        wl_resource_post_error(res, ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_DUPLICATE_FENCE,
                               "acquire fence already set for this commit");
        return;
    }
    s->pend_acquire_fd = fd;
}

static void sync_get_release(struct wl_client* c, struct wl_resource* res, uint32_t id) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) {
        wl_resource_post_error(res, ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_NO_SURFACE,
                               "the wl_surface was destroyed");
        return;
    }
    awl_esync_gc(s);
    if (s->pend_release_res) {
        wl_resource_post_error(res, ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_DUPLICATE_RELEASE,
                               "release object already requested for this commit");
        return;
    }
    struct wl_resource* rres = wl_resource_create(c, &zwp_linux_buffer_release_v1_interface, 1, id);
    struct awl_esync_release* er = calloc(1, sizeof(*er));
    if (!rres || !er) {
        if (rres) wl_resource_destroy(rres);
        free(er);
        wl_resource_post_no_memory(res);
        return;
    }
    er->res = rres;
    er->s = s;
    wl_list_insert(s->esync_all.prev, &er->all_link);
    wl_resource_set_implementation(rres, NULL, er, release_res_destroy);   /* no requests */
    s->pend_release_res = rres;
}

static const struct zwp_linux_surface_synchronization_v1_interface sync_iface = {
    .destroy = sync_destroy,
    .set_acquire_fence = sync_set_acquire_fence,
    .get_release = sync_get_release,
};

int awl_esync_commit_check(struct awl_surface* s) {
    if (s->pend_acquire_fd < 0 && !s->pend_release_res) return 0;
    struct wl_resource* err_res = s->sync_res;
    if (!s->pending_attached || !s->pending_buffer_res) {
        if (err_res)
            wl_resource_post_error(err_res, ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_NO_BUFFER,
                                   "acquire fence / release requested but no buffer attached");
        else   /* sync object already destroyed: nothing to blame, just drop the state */
            awl_surface_discard_sync(s->pend_acquire_fd, s->pend_release_res);
        s->pend_acquire_fd = -1;
        s->pend_release_res = NULL;
        return err_res ? 1 : 0;
    }
    if (s->pend_acquire_fd >= 0 && wl_shm_buffer_get(s->pending_buffer_res)) {
        if (err_res)
            wl_resource_post_error(err_res, ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_UNSUPPORTED_BUFFER,
                                   "wl_shm buffers do not support explicit synchronization");
        close(s->pend_acquire_fd);
        s->pend_acquire_fd = -1;
        return err_res ? 1 : 0;
    }
    return 0;
}

void awl_esync_surface_gone(struct awl_surface* s) {
    if (s->sync_res) {
        wl_resource_set_user_data(s->sync_res, NULL);   /* later requests → no_surface */
        s->sync_res = NULL;
    }
    if (s->pend_acquire_fd >= 0) { close(s->pend_acquire_fd); s->pend_acquire_fd = -1; }
    if (s->latched_acquire_fd >= 0) { close(s->latched_acquire_fd); s->latched_acquire_fd = -1; }
    /* every release object of this surface: never-delivered ones get an
     * immediate_release (we are done with everything), then all are
     * destroyed here on the dispatch thread */
    struct awl_esync_release* er;
    struct awl_esync_release* tmp;
    wl_list_for_each_safe(er, tmp, &s->esync_all, all_link) {
        if (!er->delivered) {
            pthread_mutex_lock(&g_bufref_lock);
            awl_esync_release_locked(er->res, -1);
            pthread_mutex_unlock(&g_bufref_lock);
        }
        wl_resource_destroy(er->res);   /* handler: unlink all_link/gc_link, free */
    }
    wl_list_init(&s->esync_all);
    wl_list_init(&s->esync_gc);
    s->pend_release_res = NULL;
    s->latched_release_res = NULL;
}

/* ---------------- factory ---------------- */

static void esync_destroy(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static void esync_get_synchronization(struct wl_client* c, struct wl_resource* res,
                                      uint32_t id, struct wl_resource* surface_res) {
    struct awl_surface* s = wl_resource_get_user_data(surface_res);
    if (s && s->sync_res) {
        wl_resource_post_error(res, ZWP_LINUX_EXPLICIT_SYNCHRONIZATION_V1_ERROR_SYNCHRONIZATION_EXISTS,
                               "surface already has a synchronization object");
        return;
    }
    struct wl_resource* sres = wl_resource_create(
            c, &zwp_linux_surface_synchronization_v1_interface,
            wl_resource_get_version(res), id);
    if (!sres) { wl_resource_post_no_memory(res); return; }
    wl_resource_set_implementation(sres, &sync_iface, s, sync_res_destroy);
    if (s) s->sync_res = sres;   /* s NULL = surface mid-destruction: object born orphaned (→ no_surface) */
}

static const struct zwp_linux_explicit_synchronization_v1_interface esync_iface = {
    .destroy = esync_destroy,
    .get_synchronization = esync_get_synchronization,
};

static void esync_bind(struct wl_client* client, void* data, uint32_t version, uint32_t id) {
    struct wl_resource* res = wl_resource_create(
            client, &zwp_linux_explicit_synchronization_v1_interface,
            version < 2 ? version : 2, id);
    if (!res) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &esync_iface, NULL, NULL);
}

void awl_esync_setup(void) {
    g_srv.g_esync = wl_global_create(g_srv.display,
                                     &zwp_linux_explicit_synchronization_v1_interface, 2,
                                     NULL, esync_bind);
}
