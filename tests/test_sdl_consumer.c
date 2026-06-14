#define _GNU_SOURCE
#include "../libdisplay_consumer/display_consumer.h"

#include <SDL2/SDL.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <linux/input-event-codes.h>

#define DEFAULT_WIDTH  1280
#define DEFAULT_HEIGHT 720
#define PIXEL_FORMAT_RGBA_8888 1

#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif
#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#endif
#ifndef EGL_DMA_BUF_PLANE0_OFFSET_EXT
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#endif
#ifndef EGL_DMA_BUF_PLANE0_PITCH_EXT
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#endif
#ifndef DRM_FORMAT_XRGB8888
#define DRM_FORMAT_XRGB8888 0x34325258
#endif

static volatile int running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

static uint32_t sdl_button_to_linux(uint8_t sdl_button)
{
    switch (sdl_button) {
    case SDL_BUTTON_LEFT:   return BTN_LEFT;
    case SDL_BUTTON_RIGHT:  return BTN_RIGHT;
    case SDL_BUTTON_MIDDLE: return BTN_MIDDLE;
    case SDL_BUTTON_X1:     return BTN_SIDE;
    case SDL_BUTTON_X2:     return BTN_EXTRA;
    default:                return BTN_LEFT;
    }
}

static void send_pointer_motion(display_ctx *ctx, float x, float y)
{
    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_MOTION,
        .pointer_motion = { .x = x, .y = y },
    };
    push_input_event(ctx, &ev);
}

static void send_pointer_button(display_ctx *ctx, uint32_t button, int pressed)
{
    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_BUTTON,
        .pointer_button = { .button = button, .pressed = pressed },
    };
    push_input_event(ctx, &ev);
}

static void send_pointer_axis(display_ctx *ctx, uint32_t axis, float value)
{
    struct InputEvent ev = {
        .type = INPUT_TYPE_POINTER_AXIS,
        .pointer_axis = { .axis = axis, .value = value, .discrete = 0 },
    };
    push_input_event(ctx, &ev);
}

static uint32_t sdl_scancode_to_linux(SDL_Scancode sc)
{
    static const uint32_t map[] = {
        [SDL_SCANCODE_A] = KEY_A, [SDL_SCANCODE_B] = KEY_B,
        [SDL_SCANCODE_C] = KEY_C, [SDL_SCANCODE_D] = KEY_D,
        [SDL_SCANCODE_E] = KEY_E, [SDL_SCANCODE_F] = KEY_F,
        [SDL_SCANCODE_G] = KEY_G, [SDL_SCANCODE_H] = KEY_H,
        [SDL_SCANCODE_I] = KEY_I, [SDL_SCANCODE_J] = KEY_J,
        [SDL_SCANCODE_K] = KEY_K, [SDL_SCANCODE_L] = KEY_L,
        [SDL_SCANCODE_M] = KEY_M, [SDL_SCANCODE_N] = KEY_N,
        [SDL_SCANCODE_O] = KEY_O, [SDL_SCANCODE_P] = KEY_P,
        [SDL_SCANCODE_Q] = KEY_Q, [SDL_SCANCODE_R] = KEY_R,
        [SDL_SCANCODE_S] = KEY_S, [SDL_SCANCODE_T] = KEY_T,
        [SDL_SCANCODE_U] = KEY_U, [SDL_SCANCODE_V] = KEY_V,
        [SDL_SCANCODE_W] = KEY_W, [SDL_SCANCODE_X] = KEY_X,
        [SDL_SCANCODE_Y] = KEY_Y, [SDL_SCANCODE_Z] = KEY_Z,
        [SDL_SCANCODE_1] = KEY_1, [SDL_SCANCODE_2] = KEY_2,
        [SDL_SCANCODE_3] = KEY_3, [SDL_SCANCODE_4] = KEY_4,
        [SDL_SCANCODE_5] = KEY_5, [SDL_SCANCODE_6] = KEY_6,
        [SDL_SCANCODE_7] = KEY_7, [SDL_SCANCODE_8] = KEY_8,
        [SDL_SCANCODE_9] = KEY_9, [SDL_SCANCODE_0] = KEY_0,
        [SDL_SCANCODE_RETURN] = KEY_ENTER,
        [SDL_SCANCODE_ESCAPE] = KEY_ESC,
        [SDL_SCANCODE_BACKSPACE] = KEY_BACKSPACE,
        [SDL_SCANCODE_TAB] = KEY_TAB,
        [SDL_SCANCODE_SPACE] = KEY_SPACE,
        [SDL_SCANCODE_MINUS] = KEY_MINUS,
        [SDL_SCANCODE_EQUALS] = KEY_EQUAL,
        [SDL_SCANCODE_LEFTBRACKET] = KEY_LEFTBRACE,
        [SDL_SCANCODE_RIGHTBRACKET] = KEY_RIGHTBRACE,
        [SDL_SCANCODE_BACKSLASH] = KEY_BACKSLASH,
        [SDL_SCANCODE_SEMICOLON] = KEY_SEMICOLON,
        [SDL_SCANCODE_APOSTROPHE] = KEY_APOSTROPHE,
        [SDL_SCANCODE_GRAVE] = KEY_GRAVE,
        [SDL_SCANCODE_COMMA] = KEY_COMMA,
        [SDL_SCANCODE_PERIOD] = KEY_DOT,
        [SDL_SCANCODE_SLASH] = KEY_SLASH,
        [SDL_SCANCODE_F1] = KEY_F1, [SDL_SCANCODE_F2] = KEY_F2,
        [SDL_SCANCODE_F3] = KEY_F3, [SDL_SCANCODE_F4] = KEY_F4,
        [SDL_SCANCODE_F5] = KEY_F5, [SDL_SCANCODE_F6] = KEY_F6,
        [SDL_SCANCODE_F7] = KEY_F7, [SDL_SCANCODE_F8] = KEY_F8,
        [SDL_SCANCODE_F9] = KEY_F9, [SDL_SCANCODE_F10] = KEY_F10,
        [SDL_SCANCODE_F11] = KEY_F11, [SDL_SCANCODE_F12] = KEY_F12,
        [SDL_SCANCODE_RIGHT] = KEY_RIGHT, [SDL_SCANCODE_LEFT] = KEY_LEFT,
        [SDL_SCANCODE_DOWN] = KEY_DOWN, [SDL_SCANCODE_UP] = KEY_UP,
        [SDL_SCANCODE_LCTRL] = KEY_LEFTCTRL,
        [SDL_SCANCODE_LSHIFT] = KEY_LEFTSHIFT,
        [SDL_SCANCODE_LALT] = KEY_LEFTALT,
        [SDL_SCANCODE_RCTRL] = KEY_RIGHTCTRL,
        [SDL_SCANCODE_RSHIFT] = KEY_RIGHTSHIFT,
        [SDL_SCANCODE_RALT] = KEY_RIGHTALT,
        [SDL_SCANCODE_DELETE] = KEY_DELETE,
        [SDL_SCANCODE_HOME] = KEY_HOME,
        [SDL_SCANCODE_END] = KEY_END,
        [SDL_SCANCODE_PAGEUP] = KEY_PAGEUP,
        [SDL_SCANCODE_PAGEDOWN] = KEY_PAGEDOWN,
        [SDL_SCANCODE_INSERT] = KEY_INSERT,
        [SDL_SCANCODE_CAPSLOCK] = KEY_CAPSLOCK,
    };
    if (sc < sizeof(map) / sizeof(map[0]))
        return map[sc];
    return KEY_UNKNOWN;
}

static void send_key(display_ctx *ctx, SDL_Scancode sc, int pressed)
{
    uint32_t code = sdl_scancode_to_linux(sc);
    if (code == KEY_UNKNOWN)
        return;
    struct InputEvent ev = {
        .type = INPUT_TYPE_KEY,
        .key = {
            .action = pressed ? INPUT_ACTION_DOWN : INPUT_ACTION_UP,
            .keycode = (int32_t)code,
        },
    };
    push_input_event(ctx, &ev);
}

int main(int argc, char **argv)
{
    const char *sock = "/tmp/display_daemon.sock";
    const char *render_node = "/dev/dri/renderD128";
    int width = DEFAULT_WIDTH;
    int height = DEFAULT_HEIGHT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
            sock = argv[++i];
        else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)
            width = atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc)
            height = atoi(argv[++i]);
        else if (strcmp(argv[i], "--gpu") == 0 && i + 1 < argc)
            render_node = argv[++i];
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    int drm_fd = open(render_node, O_RDWR);
    if (drm_fd < 0) {
        fprintf(stderr, "sdl_consumer: cannot open %s\n", render_node);
        return 1;
    }

    struct gbm_device *gbm = gbm_create_device(drm_fd);
    if (!gbm) {
        fprintf(stderr, "sdl_consumer: gbm_create_device failed\n");
        close(drm_fd);
        return 1;
    }

    struct gbm_bo *bo = gbm_bo_create(gbm, width, height,
                                       GBM_FORMAT_XRGB8888,
                                       GBM_BO_USE_RENDERING);
    if (!bo) {
        fprintf(stderr, "sdl_consumer: gbm_bo_create failed\n");
        gbm_device_destroy(gbm);
        close(drm_fd);
        return 1;
    }

    int dmabuf_fd = gbm_bo_get_fd(bo);
    uint32_t bo_stride = gbm_bo_get_stride(bo);
    uint64_t modifier = gbm_bo_get_modifier(bo);

    fprintf(stderr, "sdl_consumer: DMA-BUF fd=%d %dx%d stride=%u modifier=0x%lx\n",
            dmabuf_fd, width, height, bo_stride, (unsigned long)modifier);

    fprintf(stderr, "sdl_consumer: connecting to %s...\n", sock);

    display_ctx *ctx;
    if (connect_to_deamon(&ctx, sock) < 0) {
        fprintf(stderr, "sdl_consumer: connect failed\n");
        return 1;
    }

    set_screen_info(ctx, width, height, PIXEL_FORMAT_RGBA_8888, 0);

    struct buf_info bi = {
        .stride   = bo_stride,
        .format   = PIXEL_FORMAT_RGBA_8888,
        .modifier = modifier,
        .offset   = 0,
    };
    push_dmabufs(ctx, &dmabuf_fd, &bi, 1);

    fprintf(stderr, "sdl_consumer: connected, waiting for producer...\n");

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "sdl_consumer: SDL_Init failed: %s\n", SDL_GetError());
        disconnect(ctx);
        return 1;
    }

    SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    SDL_Window *window = SDL_CreateWindow("anland consumer",
                                          SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED,
                                          width, height, SDL_WINDOW_OPENGL);
    if (!window) {
        fprintf(stderr, "sdl_consumer: SDL_CreateWindow failed: %s\n",
                SDL_GetError());
        SDL_Quit();
        disconnect(ctx);
        return 1;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        fprintf(stderr, "sdl_consumer: SDL_GL_CreateContext failed: %s\n",
                SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        disconnect(ctx);
        return 1;
    }

    SDL_GL_SetSwapInterval(0);

    fprintf(stderr, "sdl_consumer: GL: %s / %s\n",
            glGetString(GL_VENDOR), glGetString(GL_RENDERER));

    PFNEGLCREATEIMAGEKHRPROC _eglCreateImageKHR =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC _eglDestroyImageKHR =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC _glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!_eglCreateImageKHR || !_glEGLImageTargetTexture2DOES) {
        fprintf(stderr, "sdl_consumer: required EGL extensions not available\n");
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        disconnect(ctx);
        return 1;
    }

    EGLDisplay egl_dpy = eglGetCurrentDisplay();

    EGLint img_attrs[] = {
        EGL_WIDTH, (EGLint)width,
        EGL_HEIGHT, (EGLint)height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)DRM_FORMAT_XRGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)bo_stride,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(modifier & 0xFFFFFFFF),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(modifier >> 32),
        EGL_NONE,
    };

    EGLImageKHR egl_image = _eglCreateImageKHR(egl_dpy, EGL_NO_CONTEXT,
                                                 EGL_LINUX_DMA_BUF_EXT,
                                                 NULL, img_attrs);
    if (egl_image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "sdl_consumer: eglCreateImageKHR failed: 0x%x\n",
                eglGetError());
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        disconnect(ctx);
        return 1;
    }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    _glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)egl_image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    GLuint read_fbo;
    glGenFramebuffers(1, &read_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, read_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    GLenum fbo_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fbo_status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "sdl_consumer: FBO incomplete: 0x%x\n", fbo_status);
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        disconnect(ctx);
        return 1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    fprintf(stderr, "sdl_consumer: DMA-BUF imported as GL texture, FBO ready\n");

    int frame_count = 0;
    Uint32 last_fps_time = SDL_GetTicks();
    int fps_frames = 0;

    fprintf(stderr, "sdl_consumer: entering main loop\n");

    while (running) {
        SDL_Event sdl_ev;
        while (SDL_PollEvent(&sdl_ev)) {
            switch (sdl_ev.type) {
            case SDL_QUIT:
                running = 0;
                break;
            case SDL_MOUSEMOTION:
                send_pointer_motion(ctx, (float)sdl_ev.motion.x,
                                    (float)sdl_ev.motion.y);
                break;
            case SDL_MOUSEBUTTONDOWN:
                send_pointer_button(ctx,
                                    sdl_button_to_linux(sdl_ev.button.button), 1);
                break;
            case SDL_MOUSEBUTTONUP:
                send_pointer_button(ctx,
                                    sdl_button_to_linux(sdl_ev.button.button), 0);
                break;
            case SDL_MOUSEWHEEL:
                if (sdl_ev.wheel.y != 0)
                    send_pointer_axis(ctx, 0, (float)(-sdl_ev.wheel.y * 10));
                if (sdl_ev.wheel.x != 0)
                    send_pointer_axis(ctx, 1, (float)(sdl_ev.wheel.x * 10));
                break;
            case SDL_KEYDOWN:
                if (!sdl_ev.key.repeat)
                    send_key(ctx, sdl_ev.key.keysym.scancode, 1);
                break;
            case SDL_KEYUP:
                send_key(ctx, sdl_ev.key.keysym.scancode, 0);
                break;
            }
        }

        if (select_dmabuf(ctx, 0) < 0) {
            SDL_Delay(16);
            continue;
        }

        if (refresh_done(ctx) < 0) {
            SDL_Delay(16);
            continue;
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, width, height,
                          0, height, width, 0,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        SDL_GL_SwapWindow(window);

        frame_count++;
        fps_frames++;

        Uint32 now = SDL_GetTicks();
        if (now - last_fps_time >= 3000) {
            float fps = fps_frames * 1000.0f / (now - last_fps_time);
            fprintf(stderr, "sdl_consumer: %d frames, %.1f fps\n",
                    frame_count, fps);
            fps_frames = 0;
            last_fps_time = now;
        }
    }

    glDeleteFramebuffers(1, &read_fbo);
    glDeleteTextures(1, &tex);
    if (_eglDestroyImageKHR)
        _eglDestroyImageKHR(egl_dpy, egl_image);
    close(dmabuf_fd);
    gbm_bo_destroy(bo);
    gbm_device_destroy(gbm);
    close(drm_fd);
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    disconnect(ctx);
    fprintf(stderr, "sdl_consumer: exit\n");
    return 0;
}
