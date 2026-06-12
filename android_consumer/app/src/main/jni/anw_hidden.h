#ifndef ANW_HIDDEN_H
#define ANW_HIDDEN_H

#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>

/* native_handle_t layout (stable ABI from cutils/native_handle.h) */
typedef struct {
    int version;
    int numFds;
    int numInts;
    int data[];
} native_handle_compat_t;

/* ANativeWindowBuffer layout (stable ABI from nativebase.h)
   Only fields up to `handle` are needed. */
typedef struct {
    /* android_native_base_t: magic(4) + version(4) + reserved(4*ptr) + incRef(ptr) + decRef(ptr) */
    char _native_base[sizeof(void *) == 8 ? 48 : 28];
    int width;
    int height;
    int stride;
    int format;
    int usage_deprecated;
    uintptr_t layerCount;
    void *reserved_0;
    const native_handle_compat_t *handle;
} ANativeWindowBuffer_compat;

/* Query constants */
#define ANATIVEWINDOW_QUERY_MIN_UNDEQUEUED_BUFFERS 3

/* Hidden API function pointers, resolved via dlsym */
typedef int (*pfn_ANativeWindow_dequeueBuffer)(ANativeWindow *, ANativeWindowBuffer_compat **, int *);
typedef int (*pfn_ANativeWindow_queueBuffer)(ANativeWindow *, ANativeWindowBuffer_compat *, int);
typedef int (*pfn_ANativeWindow_cancelBuffer)(ANativeWindow *, ANativeWindowBuffer_compat *, int);
typedef int (*pfn_ANativeWindow_setBufferCount)(ANativeWindow *, size_t);
typedef int (*pfn_ANativeWindow_setUsage)(ANativeWindow *, uint64_t);
typedef int (*pfn_ANativeWindow_query)(const ANativeWindow *, int, int *);
struct anw_api {
    pfn_ANativeWindow_dequeueBuffer dequeueBuffer;
    pfn_ANativeWindow_queueBuffer   queueBuffer;
    pfn_ANativeWindow_cancelBuffer  cancelBuffer;
    pfn_ANativeWindow_setBufferCount setBufferCount;
    pfn_ANativeWindow_setUsage      setUsage;
    pfn_ANativeWindow_query         query;
};

static inline int anw_api_load(struct anw_api *api)
{
    void *lib = dlopen("libnativewindow.so", RTLD_NOW);
    if (!lib)
        return -1;

    api->dequeueBuffer  = (pfn_ANativeWindow_dequeueBuffer) dlsym(lib, "ANativeWindow_dequeueBuffer");
    api->queueBuffer    = (pfn_ANativeWindow_queueBuffer)   dlsym(lib, "ANativeWindow_queueBuffer");
    api->cancelBuffer   = (pfn_ANativeWindow_cancelBuffer)  dlsym(lib, "ANativeWindow_cancelBuffer");
    api->setBufferCount = (pfn_ANativeWindow_setBufferCount) dlsym(lib, "ANativeWindow_setBufferCount");
    api->setUsage       = (pfn_ANativeWindow_setUsage)      dlsym(lib, "ANativeWindow_setUsage");
    api->query          = (pfn_ANativeWindow_query)         dlsym(lib, "ANativeWindow_query");

    if (!api->dequeueBuffer || !api->queueBuffer || !api->setBufferCount ||
        !api->setUsage || !api->query)
        return -1;

    return 0;
}

#endif
