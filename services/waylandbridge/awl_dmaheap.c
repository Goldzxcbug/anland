/* awl_dmaheap.c — CPU-writable dma-buf allocation for the shm→dmabuf path
 * (awl_shmblit.c), straight from the kernel DMA heap uapi: no gralloc, no
 * AHardwareBuffer — the logic layer stays free of Android objects and the
 * resulting fd travels through awl_bufferqueue exactly like a client's
 * zwp_linux_dmabuf buffer (the renderer imports it the same way).
 *
 * Heap: /dev/dma_heap/system (the generic cached system heap; the qcom
 * variant is tried second). Root daemon → the node is accessible directly.
 * CPU access is bracketed with DMA_BUF_IOCTL_SYNC so the cached heap's lines
 * are written back before the GPU samples the buffer. */
#include "awl_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int heap_fd(void) {
    static atomic_int fd_cache = -2;   /* -2 = not tried, -1 = unavailable */
    int fd = atomic_load(&fd_cache);
    if (fd != -2) return fd;
    static const char* nodes[] = { "/dev/dma_heap/system", "/dev/dma_heap/qcom,system" };
    fd = -1;
    for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]) && fd < 0; i++) {
        fd = open(nodes[i], O_RDONLY | O_CLOEXEC);
        if (fd >= 0) LOGI("dma heap: %s", nodes[i]);
    }
    if (fd < 0) LOGE("dma heap: no usable /dev/dma_heap node (%s) — shm surfaces cannot be converted", strerror(errno));
    int expect = -2;
    if (!atomic_compare_exchange_strong(&fd_cache, &expect, fd)) {   /* lost the race: keep the winner's */
        if (fd >= 0) close(fd);
        fd = atomic_load(&fd_cache);
    }
    return fd;
}

int awl_dmaheap_alloc(size_t len) {
    int hf = heap_fd();
    if (hf < 0) return -1;
    struct dma_heap_allocation_data arg;
    memset(&arg, 0, sizeof(arg));
    arg.len = len;
    arg.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(hf, DMA_HEAP_IOCTL_ALLOC, &arg) != 0) {
        LOGE("dma heap alloc %zu: %s", len, strerror(errno));
        return -1;
    }
    return (int)arg.fd;
}

void* awl_dmabuf_map(int fd, size_t len) {
    void* p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        LOGE("dma-buf mmap %zu: %s", len, strerror(errno));
        return NULL;
    }
    return p;
}

void awl_dmabuf_unmap(void* p, size_t len) {
    if (p) munmap(p, len);
}

int awl_dmabuf_cpu_sync(int fd, int start, int write) {
    struct dma_buf_sync s = { .flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) |
                                       (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ) };
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) == 0 ? 0 : -1;
}
