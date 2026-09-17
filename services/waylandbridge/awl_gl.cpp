/* awl_gl.cpp — shared EGL/GLES glue, see awl_gl.hpp. */
#include "awl_gl.hpp"
#include "awl.h"               /* awl_shm_frame_t */
#include "awl_ahb.hpp"
#include "awl_bufferqueue.h"   /* awl_fence_wait (CPU fallback) */

#include <string.h>
#include <unistd.h>

#include <mutex>

#define AWL_TAG "anland-gl"
#include "awl_log.h"

/* ---------------- display / procs ---------------- */

static struct {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = 0;
    bool inited = false;
    bool surfaceless = false;   /* EGL_KHR_surfaceless_context */
    bool native_fence = false;  /* EGL_ANDROID_native_fence_sync */
    bool wait_sync = false;     /* EGL_KHR_wait_sync */
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = nullptr;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC eglGetNativeClientBuffer = nullptr;
    PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR = nullptr;
    PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR = nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID = nullptr;
    PFNEGLWAITSYNCKHRPROC eglWaitSyncKHR = nullptr;
} g;

bool awl_gl_init(void) {
    if (g.inited) return true;
    static std::mutex init_lock;
    std::lock_guard<std::mutex> lk(init_lock);
    if (g.inited) return true;
    g.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g.display == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }
    if (!eglInitialize(g.display, NULL, NULL)) {
        LOGE("eglInitialize: 0x%x", eglGetError());
        return false;
    }
    const char* exts = eglQueryString(g.display, EGL_EXTENSIONS);
    LOGI("EGL extensions: %s", exts ? exts : "none");
    bool has_dmabuf = exts && strstr(exts, "EGL_EXT_image_dma_buf_import");
    bool has_img2d = exts && strstr(exts, "EGL_KHR_gl_texture_2D_image");
    if (!has_dmabuf || !has_img2d)
        LOGE("!! dmabuf import %s / image2d %s",
             has_dmabuf ? "ok" : "MISSING", has_img2d ? "ok" : "MISSING");

    g.eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    g.eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    g.glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    g.eglGetNativeClientBuffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)
        eglGetProcAddress("eglGetNativeClientBufferANDROID");
    if (!g.eglCreateImageKHR || !g.eglDestroyImageKHR ||
        !g.glEGLImageTargetTexture2DOES || !g.eglGetNativeClientBuffer) {
        LOGE("EGLImage procs missing");
        return false;
    }
    if (exts && strstr(exts, "EGL_ANDROID_native_fence_sync")) {
        g.eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
        g.eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
        g.eglDupNativeFenceFDANDROID = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)
            eglGetProcAddress("eglDupNativeFenceFDANDROID");
        g.native_fence = g.eglCreateSyncKHR && g.eglDestroySyncKHR && g.eglDupNativeFenceFDANDROID;
    }
    if (g.native_fence && exts && strstr(exts, "EGL_KHR_wait_sync")) {
        g.eglWaitSyncKHR = (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
        g.wait_sync = g.eglWaitSyncKHR != nullptr;
    }
    g.surfaceless = exts && strstr(exts, "EGL_KHR_surfaceless_context");
    LOGI("EGL native fence sync: %s, wait_sync: %s, surfaceless: %s",
         g.native_fence ? "yes" : "NO — immediate releases",
         g.wait_sync ? "yes" : "no (CPU waits)", g.surfaceless ? "yes" : "no (pbuffer)");

    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0,
        EGL_NONE,
    };
    EGLint n = 0;
    if (!eglChooseConfig(g.display, cfg_attr, &g.config, 1, &n) || n < 1) {
        LOGE("eglChooseConfig: 0x%x n=%d", eglGetError(), n);
        return false;
    }
    g.inited = true;
    return true;
}

EGLDisplay awl_gl_display(void) { return g.display; }
EGLConfig awl_gl_config(void) { return g.config; }
bool awl_gl_has_native_fence(void) { return g.native_fence; }

EGLContext awl_gl_create_context(void) {
    if (!awl_gl_init()) return EGL_NO_CONTEXT;
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext c = eglCreateContext(g.display, g.config, EGL_NO_CONTEXT, ctx_attr);
    if (c == EGL_NO_CONTEXT) LOGE("eglCreateContext: 0x%x", eglGetError());
    return c;
}

bool awl_gl_make_current_offscreen(EGLContext ctx, EGLSurface* pbuf) {
    *pbuf = EGL_NO_SURFACE;
    if (g.surfaceless &&
        eglMakeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
        return true;
    const EGLint attr[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface(g.display, g.config, attr);
    if (s == EGL_NO_SURFACE) {
        LOGE("eglCreatePbufferSurface: 0x%x", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(g.display, s, s, ctx)) {
        LOGE("eglMakeCurrent(pbuffer): 0x%x", eglGetError());
        eglDestroySurface(g.display, s);
        return false;
    }
    *pbuf = s;
    return true;
}

/* ---------------- layer quad ---------------- */

static const char* k_vert_src =
    "#version 300 es\n"
    "in vec2 pos;\n"
    "uniform vec2 u_view;   /* target px */\n"
    "uniform vec4 u_dst;    /* dst rect px: x,y=top-left w,h=size (Y down) */\n"
    "uniform vec4 u_uv;     /* sample region transform: uv = u_uv.xy + uv*u_uv.zw (#31 source) */\n"
    "uniform mat3 u_xform;  /* display uv q -> buffer uv (wl_surface.set_buffer_transform) */\n"
    "uniform float u_flip;  /* +1 window surface (GL origin bottom-left), -1 FBO texture (row 0 = top) */\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "  vec2 px = vec2(u_dst.x + (pos.x * 0.5 + 0.5) * u_dst.z,\n"
    "                 u_dst.y + (0.5 - pos.y * 0.5) * u_dst.w);\n"
    "  gl_Position = vec4(px.x / u_view.x * 2.0 - 1.0,\n"
    "                     u_flip * (1.0 - px.y / u_view.y * 2.0), 0.0, 1.0);\n"
    "  vec2 q = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);\n"
    "  uv = u_uv.xy + (u_xform * vec3(q, 1.0)).xy * u_uv.zw;\n"
    "}\n";

static const char* k_frag_src =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 uv;\n"
    "out vec4 color;\n"
    "uniform sampler2D tex;\n"
    "void main() {\n"
    "  color = texture(tex, uv);\n"   /* channel order comes from the texture format (dmabuf = BGRA_8888) */
    "}\n";

const float awl_gl_xform[8][9] = {
    /* 0 normal      */ { 1, 0, 0,   0, 1, 0,   0, 0, 1 },
    /* 1 90          */ { 0, 1, 0,  -1, 0, 1,   0, 0, 1 },
    /* 2 180         */ { -1, 0, 1,  0, -1, 1,  0, 0, 1 },
    /* 3 270         */ { 0, -1, 1,  1, 0, 0,   0, 0, 1 },
    /* 4 flipped     */ { -1, 0, 1,  0, 1, 0,   0, 0, 1 },
    /* 5 flipped_90  */ { 0, 1, 0,   1, 0, 0,   0, 0, 1 },
    /* 6 flipped_180 */ { 1, 0, 0,   0, -1, 1,  0, 0, 1 },
    /* 7 flipped_270 */ { 0, -1, 1, -1, 0, 1,   0, 0, 1 },
};

static GLuint compile(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        LOGE("shader: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

bool awl_gl_quad_create(awl_gl_quad* q) {
    GLuint vs = compile(GL_VERTEX_SHADER, k_vert_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, k_frag_src);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    q->program = glCreateProgram();
    glAttachShader(q->program, vs);
    glAttachShader(q->program, fs);
    glLinkProgram(q->program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(q->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        LOGE("program link failed");
        glDeleteProgram(q->program);
        q->program = 0;
        return false;
    }
    /* attribute/uniform locations — fixed at link time, resolved once
     * (glGet*Location is a driver string lookup per call) */
    q->a_pos = glGetAttribLocation(q->program, "pos");
    q->u_view = glGetUniformLocation(q->program, "u_view");
    q->u_dst = glGetUniformLocation(q->program, "u_dst");
    q->u_uv = glGetUniformLocation(q->program, "u_uv");
    q->u_xform = glGetUniformLocation(q->program, "u_xform");
    q->u_flip = glGetUniformLocation(q->program, "u_flip");
    q->u_tex = glGetUniformLocation(q->program, "tex");

    static const float quad[] = {
        -1, -1,  1, -1,  -1, 1,
        -1,  1,  1, -1,   1, 1,
    };
    glGenBuffers(1, &q->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, q->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    return true;
}

void awl_gl_quad_destroy(awl_gl_quad* q) {
    if (q->program) glDeleteProgram(q->program);
    if (q->vbo) glDeleteBuffers(1, &q->vbo);
    *q = awl_gl_quad();
}

void awl_gl_quad_begin(const awl_gl_quad* q, float view_w, float view_h, float flip) {
    glUseProgram(q->program);
    glBindBuffer(GL_ARRAY_BUFFER, q->vbo);
    glEnableVertexAttribArray((GLuint)q->a_pos);
    glVertexAttribPointer((GLuint)q->a_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glUniform2f(q->u_view, view_w, view_h);
    glUniform1f(q->u_flip, flip);
    glUniform1i(q->u_tex, 0);
    glActiveTexture(GL_TEXTURE0);
}

void awl_gl_quad_draw(const awl_gl_quad* q, GLuint tex, const float dst[4],
                      const float uv[4], int transform) {
    glUniform4f(q->u_dst, dst[0], dst[1], dst[2], dst[3]);
    glUniform4f(q->u_uv, uv[0], uv[1], uv[2], uv[3]);
    glUniformMatrix3fv(q->u_xform, 1, GL_TRUE, awl_gl_xform[transform & 7]);
    glBindTexture(GL_TEXTURE_2D, tex);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* ---------------- AHB → texture ---------------- */

bool awl_gl_tex_import(AHardwareBuffer* ahb, awl_gl_tex* out) {
    EGLClientBuffer cb = g.eglGetNativeClientBuffer(ahb);   /* AHB → EGLClientBuffer (the sanctioned path) */
    if (!cb) {
        LOGE("eglGetNativeClientBuffer == NULL");
        return false;
    }
    EGLImageKHR img = g.eglCreateImageKHR(g.display, EGL_NO_CONTEXT,
                                          EGL_NATIVE_BUFFER_ANDROID, cb, NULL);
    if (img == EGL_NO_IMAGE_KHR) {
        AHardwareBuffer_Desc d;
        AHardwareBuffer_describe(ahb, &d);
        LOGE("eglCreateImageKHR(native buffer): 0x%x (%ux%u stride=%u fmt=%u usage=0x%llx)",
             eglGetError(), d.width, d.height, d.stride, d.format, (unsigned long long)d.usage);
        return false;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
    GLenum terr = glGetError();
    if (terr != GL_NO_ERROR) {
        LOGE("EGLImageTargetTexture: 0x%x", terr);
        glDeleteTextures(1, &tex);
        g.eglDestroyImageKHR(g.display, img);
        return false;
    }
    out->image = img;
    out->texture = tex;
    return true;
}

void awl_gl_tex_release(awl_gl_tex* t) {
    if (t->texture) glDeleteTextures(1, &t->texture);
    if (t->image != EGL_NO_IMAGE_KHR) g.eglDestroyImageKHR(g.display, t->image);
    *t = awl_gl_tex();
}

void awl_gl_tex_payload_destroy(void* payload) {
    awl_gl_tex* t = (awl_gl_tex*)payload;
    awl_gl_tex_release(t);
    delete t;
}

GLuint awl_gl_slot_texture(struct awl_ahb_slot* s) {
    awl_gl_tex* t = (awl_gl_tex*)s->payload;
    if (!t) {   /* fresh forge: EGLImage + texture on it */
        t = new awl_gl_tex();
        if (!awl_gl_tex_import(s->ahb, t)) {
            delete t;
            return 0;
        }
        s->payload = t;
        LOGD("AHB import ok %ux%u stride=%u ino=%llu", s->w, s->h, s->stride,
             (unsigned long long)s->ino);
    }
    return t->texture;
}

/* ---------------- wl_shm → texture ---------------- */

static bool gl_has_bgra_upload(void) {
    static int cached = -1;   /* per process; every context here is the same driver */
    if (cached < 0) {
        const char* e = (const char*)glGetString(GL_EXTENSIONS);
        cached = (e && strstr(e, "GL_EXT_texture_format_BGRA8888")) ? 1 : 0;
        if (!cached) LOGI("GL_EXT_texture_format_BGRA8888 missing — shm uploads swizzle RGBA");
    }
    return cached == 1;
}

bool awl_gl_shm_update(awl_gl_shm_tex* t, const struct awl_shm_frame* f) {
    if (!f->pixels || !f->width || !f->height || f->stride < f->width * 4) return false;
    bool bgra = gl_has_bgra_upload();
    GLenum fmt = bgra ? GL_BGRA_EXT : GL_RGBA;
    bool fresh = !t->texture || t->w != f->width || t->h != f->height || t->format != f->format;
    if (fresh) {
        if (!t->texture) glGenTextures(1, &t->texture);
        glBindTexture(GL_TEXTURE_2D, t->texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (!bgra) {   /* memory B,G,R,A read as RGBA → swap R and B at sampling */
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        }
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, (GLsizei)f->width, (GLsizei)f->height, 0,
                     fmt, GL_UNSIGNED_BYTE, NULL);
        t->w = f->width;
        t->h = f->height;
        t->format = f->format;
    } else {
        glBindTexture(GL_TEXTURE_2D, t->texture);
    }
    /* upload rect: everything for a fresh texture / full damage, else the
     * damage bbox clipped to the buffer; nothing recorded → nothing to copy */
    int32_t x = 0, y = 0, w = (int32_t)f->width, h = (int32_t)f->height;
    if (!fresh && !f->dmg_full) {
        x = f->dmg_x < 0 ? 0 : f->dmg_x;
        y = f->dmg_y < 0 ? 0 : f->dmg_y;
        int32_t x2 = f->dmg_x + f->dmg_w, y2 = f->dmg_y + f->dmg_h;
        if (x2 > (int32_t)f->width) x2 = (int32_t)f->width;
        if (y2 > (int32_t)f->height) y2 = (int32_t)f->height;
        w = x2 - x;
        h = y2 - y;
    }
    if (w > 0 && h > 0) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(f->stride / 4));
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, fmt, GL_UNSIGNED_BYTE,
                        (const uint8_t*)f->pixels + (size_t)y * f->stride + (size_t)x * 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        LOGE("shm upload %ux%u stride=%u rect %d,%d %dx%d: gl error 0x%x",
             f->width, f->height, f->stride, x, y, w, h, err);
        return false;
    }
    t->serial = f->serial;
    return true;
}

void awl_gl_shm_release(awl_gl_shm_tex* t) {
    if (t->texture) glDeleteTextures(1, &t->texture);
    *t = awl_gl_shm_tex();
}

/* ---------------- fences ---------------- */

int awl_gl_fence_fd(void) {
    if (!g.native_fence) return -1;
    EGLSyncKHR sync = g.eglCreateSyncKHR(g.display, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (sync == EGL_NO_SYNC_KHR) return -1;
    int fd = g.eglDupNativeFenceFDANDROID(g.display, sync);   /* flushes */
    g.eglDestroySyncKHR(g.display, sync);
    return fd >= 0 ? fd : -1;
}

void awl_gl_wait_fence_fd(int fd) {
    if (fd < 0) return;
    if (g.wait_sync) {
        const EGLint attr[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE };
        EGLSyncKHR sync = g.eglCreateSyncKHR(g.display, EGL_SYNC_NATIVE_FENCE_ANDROID, attr);
        if (sync != EGL_NO_SYNC_KHR) {   /* EGL owns fd from here */
            g.eglWaitSyncKHR(g.display, sync, 0);
            g.eglDestroySyncKHR(g.display, sync);
            return;
        }
    }
    awl_fence_wait(fd, 100);   /* CPU fallback: bounded, then proceed */
    close(fd);
}
