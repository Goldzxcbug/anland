#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/eventfd.h>
#include <stdbool.h>
#include <linux/input.h>

#include <libweston/libweston.h>
#include <libweston/backend-virtual-drm.h>
#include "shared/helpers.h"
#include "pixel-formats.h"
#include "renderer-gl/gl-renderer.h"
#include "renderer-vulkan/vulkan-renderer.h"
#include "linux-dmabuf.h"
#include "shared/weston-drm-fourcc.h"
#include "shared/weston-egl-ext.h"
#include "shared/xalloc.h"
#include "shared/timespec-util.h"
#include "presentation-time-server-protocol.h"
#include <libweston/windowed-output-api.h>

#include "display_producer.h"

#define DEFAULT_SOCKET_PATH "/tmp/display_daemon.sock"
#define DEFAULT_REFRESH 60000

static const uint32_t vdrm_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

struct vdrm_backend {
	struct weston_backend base;
	struct weston_compositor *compositor;

	struct weston_seat seat;

	display_ctx *display;
	char *socket_path;
	int refresh;
	int screen_w;
	int screen_h;

	struct wl_event_source *input_source;
	struct weston_touch_device *touch_device;

	struct wl_event_source *buf_ready_source;
	bool consumer_ready;
	bool dmabuf_received;
	bool in_fallback;

	const struct pixel_format_info **formats;
	unsigned int formats_count;
};

struct vdrm_head {
	struct weston_head base;
};

struct vdrm_output {
	struct weston_output base;
	struct vdrm_backend *backend;

	struct weston_mode mode;
	struct wl_event_source *finish_frame_timer;

	int buf_count;
	weston_renderbuffer_t renderbuffers[MAX_BUFS];
	struct dmabuf_attributes dmabuf_attrs_arr[MAX_BUFS];
	struct linux_dmabuf_memory dmabuf_mem_arr[MAX_BUFS];
};

static void
vdrm_destroy(struct weston_backend *backend);

static inline struct vdrm_head *
to_vdrm_head(struct weston_head *base)
{
	if (base->backend->destroy != vdrm_destroy)
		return NULL;
	return container_of(base, struct vdrm_head, base);
}

static void
vdrm_output_destroy(struct weston_output *base);

static inline struct vdrm_output *
to_vdrm_output(struct weston_output *base)
{
	if (base->destroy != vdrm_output_destroy)
		return NULL;
	return container_of(base, struct vdrm_output, base);
}

static inline struct vdrm_backend *
to_vdrm_backend(struct weston_backend *base)
{
	return container_of(base, struct vdrm_backend, base);
}

static void
vdrm_handle_pointer_motion(struct vdrm_backend *b, const struct InputEvent *ev)
{
	struct timespec ts;
	struct weston_pointer_motion_event motion;
	struct weston_coord_global abs;
	struct weston_output *output;

	weston_compositor_get_time(&ts);

	output = weston_compositor_find_output_by_name(b->compositor,
						       "virtual-drm-1");
	if (!output)
		return;

	abs = weston_coord_global_from_output_point(ev->pointer_motion.x,
						    ev->pointer_motion.y,
						    output);

	weston_pointer_motion_event_init(&motion, &ts, &b->seat,
					 WESTON_POINTER_MOTION_ABS,
					 &abs, NULL, NULL);
	notify_motion(&motion);
	notify_pointer_frame(&b->seat);
}

static void
vdrm_handle_pointer_button(struct vdrm_backend *b, const struct InputEvent *ev)
{
	struct timespec ts;
	struct weston_pointer_button_event btn;

	weston_compositor_get_time(&ts);
	weston_pointer_button_event_init(&btn, &ts, &b->seat,
					 ev->pointer_button.button,
					 ev->pointer_button.pressed ?
					 WL_POINTER_BUTTON_STATE_PRESSED :
					 WL_POINTER_BUTTON_STATE_RELEASED);
	notify_button(&btn);
	notify_pointer_frame(&b->seat);
}

static void
vdrm_handle_pointer_axis(struct vdrm_backend *b, const struct InputEvent *ev)
{
	struct timespec ts;
	struct weston_pointer_axis_event axis;

	weston_compositor_get_time(&ts);
	weston_pointer_axis_event_init(&axis, &ts, &b->seat,
				       ev->pointer_axis.axis,
				       (double)ev->pointer_axis.value,
				       ev->pointer_axis.discrete != 0,
				       ev->pointer_axis.discrete);
	notify_axis(&axis);
	notify_pointer_frame(&b->seat);
}

static void
vdrm_handle_key(struct vdrm_backend *b, const struct InputEvent *ev)
{
	struct timespec ts;
	struct weston_key_event key;

	weston_compositor_get_time(&ts);
	weston_key_event_init(&key, &ts, &b->seat,
			      ev->key.keycode,
			      ev->key.action == INPUT_ACTION_DOWN ?
			      WL_KEYBOARD_KEY_STATE_PRESSED :
			      WL_KEYBOARD_KEY_STATE_RELEASED,
			      STATE_UPDATE_AUTOMATIC);
	notify_key(&key);
}

static void
vdrm_handle_touch(struct vdrm_backend *b, const struct InputEvent *ev)
{
	struct timespec ts;
	struct weston_touch_event touch;
	struct weston_output *output;
	int32_t type;

	if (!b->touch_device)
		return;

	weston_compositor_get_time(&ts);

	output = weston_compositor_find_output_by_name(b->compositor,
						       "virtual-drm-1");
	if (!output)
		return;

	switch (ev->touch.action) {
	case INPUT_ACTION_DOWN:
		type = WL_TOUCH_DOWN;
		break;
	case INPUT_ACTION_UP:
		type = WL_TOUCH_UP;
		break;
	case INPUT_ACTION_MOVE:
		type = WL_TOUCH_MOTION;
		break;
	default:
		return;
	}

	struct weston_coord_global pos =
		weston_coord_global_from_output_point(ev->touch.x, ev->touch.y,
						      output);

	weston_touch_event_init(&touch, &ts, &b->seat, b->touch_device,
				type, ev->touch.pointer_id, &pos);
	notify_touch_normalized(&touch, NULL);
	notify_touch_frame(b->touch_device);
}

static void
vdrm_process_input_event(struct vdrm_backend *b, const struct InputEvent *ev)
{
	switch (ev->type) {
	case INPUT_TYPE_POINTER_MOTION:
		vdrm_handle_pointer_motion(b, ev);
		break;
	case INPUT_TYPE_POINTER_BUTTON:
		vdrm_handle_pointer_button(b, ev);
		break;
	case INPUT_TYPE_POINTER_AXIS:
		vdrm_handle_pointer_axis(b, ev);
		break;
	case INPUT_TYPE_KEY:
		vdrm_handle_key(b, ev);
		break;
	case INPUT_TYPE_TOUCH:
		vdrm_handle_touch(b, ev);
		break;
	}
}

static int
vdrm_buf_ready_handler(int fd, uint32_t mask, void *data);

static void
vdrm_fallback_cb(void *data)
{
	struct vdrm_backend *b = data;

	weston_log("virtual-drm: consumer disconnected, entering fallback\n");

	if (b->buf_ready_source) {
		wl_event_source_remove(b->buf_ready_source);
		b->buf_ready_source = NULL;
	}
	b->consumer_ready = false;
	b->dmabuf_received = false;
	b->in_fallback = true;
}

static void
vdrm_drop_renderbuffers(struct vdrm_backend *b)
{
	struct weston_output *output;

	wl_list_for_each(output, &b->compositor->output_list, link) {
		struct vdrm_output *vout = to_vdrm_output(output);
		if (vout) {
			memset(vout->renderbuffers, 0,
			       sizeof(vout->renderbuffers));
			vout->buf_count = 0;
			break;
		}
	}
}

static void
vdrm_try_reconnect(struct vdrm_backend *b)
{
	if (try_reconnect(b->display) < 0)
		return;

	weston_log("virtual-drm: consumer reconnected\n");
	b->in_fallback = false;

	int buf_ready_fd = get_buffer_ready_fd(b->display);
	if (buf_ready_fd >= 0) {
		struct wl_event_loop *loop =
			wl_display_get_event_loop(b->compositor->wl_display);
		b->buf_ready_source = wl_event_loop_add_fd(loop, buf_ready_fd,
							    WL_EVENT_READABLE,
							    vdrm_buf_ready_handler, b);
	}
}

static int
vdrm_input_timer_handler(void *data)
{
	struct vdrm_backend *b = data;
	struct InputEvent ev;

	if (b->in_fallback) {
		vdrm_drop_renderbuffers(b);
		vdrm_try_reconnect(b);
	} else {
		while (poll_input_event(b->display, &ev, 0) > 0)
			vdrm_process_input_event(b, &ev);
	}

	if (b->input_source)
		wl_event_source_timer_update(b->input_source, 8);

	return 0;
}

static int
vdrm_output_start_repaint_loop(struct weston_output *output)
{
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->compositor, &ts);
	weston_output_finish_frame(output, &ts, WP_PRESENTATION_FEEDBACK_INVALID);

	return 0;
}

static int
finish_frame_handler(void *data)
{
	struct vdrm_output *output = data;

	weston_output_finish_frame_from_timer(&output->base);

	if (output->backend->consumer_ready)
		weston_output_schedule_repaint(&output->base);

	return 1;
}

static uint32_t
protocol_format_to_drm(uint32_t fmt)
{
	switch (fmt) {
	case 1: /* PIXEL_FORMAT_RGBA_8888 */
		return DRM_FORMAT_XRGB8888;
	default:
		return DRM_FORMAT_XRGB8888;
	}
}

static int
vdrm_output_import_dmabufs(struct vdrm_output *output)
{
	struct vdrm_backend *b = output->backend;
	struct weston_renderer *renderer = b->compositor->renderer;
	int count = get_buf_count(b->display);
	const struct weston_mode *mode = output->base.current_mode;

	if (count <= 0)
		return -1;

	for (int i = 0; i < count; i++) {
		int fd = get_dmabuf_fd_at(b->display, i);
		struct buf_info info;
		if (fd < 0 || get_dmabuf_info_at(b->display, i, &info) < 0)
			return -1;

		struct dmabuf_attributes *attrs = &output->dmabuf_attrs_arr[i];
		memset(attrs, 0, sizeof(*attrs));
		attrs->width = mode->width;
		attrs->height = mode->height;
		attrs->format = protocol_format_to_drm(info.format);
		attrs->n_planes = 1;
		attrs->fd[0] = fd;
		attrs->offset[0] = info.offset;
		attrs->stride[0] = info.stride;
		attrs->modifier = info.modifier;

		output->dmabuf_mem_arr[i].attributes = attrs;
		output->dmabuf_mem_arr[i].destroy = NULL;

		weston_log("virtual-drm: importing buf[%d] fd=%d %dx%d stride=%u fmt=0x%x mod=0x%lx\n",
			   i, fd, mode->width, mode->height,
			   info.stride, attrs->format,
			   (unsigned long)info.modifier);

		output->renderbuffers[i] =
			renderer->create_renderbuffer_dmabuf(&output->base,
							     &output->dmabuf_mem_arr[i],
							     NULL, NULL);
		if (!output->renderbuffers[i]) {
			weston_log("virtual-drm: failed to create renderbuffer[%d]\n", i);
			return -1;
		}
	}
	output->buf_count = count;
	return 0;
}

static int
vdrm_buf_ready_handler(int fd, uint32_t mask, void *data)
{
	struct vdrm_backend *b = data;

	if (!b->dmabuf_received) {
		wait_buffer_async(b->display);
		void *buf;
		if (wait_buffer_async_result(b->display, &buf) != 0)
			return 0;
		b->dmabuf_received = true;

		struct weston_output *output;
		wl_list_for_each(output, &b->compositor->output_list, link) {
			struct vdrm_output *vout = to_vdrm_output(output);
			if (vout) {
				if (vdrm_output_import_dmabufs(vout) < 0)
					weston_log("virtual-drm: dmabuf import failed\n");
				break;
			}
		}
	} else {
		eventfd_t val;
		eventfd_read(fd, &val);
	}

	b->consumer_ready = true;

	struct weston_output *output;
	wl_list_for_each(output, &b->compositor->output_list, link) {
		if (to_vdrm_output(output)) {
			weston_output_schedule_repaint(output);
			break;
		}
	}

	return 0;
}

static void
finish_frame_idle(void *data)
{
	struct vdrm_output *output = data;
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->base.compositor, &ts);
	weston_output_finish_frame(&output->base, &ts, 0);
}

static int
vdrm_output_repaint(struct weston_output *output_base)
{
	struct vdrm_output *output = to_vdrm_output(output_base);
	struct vdrm_backend *b = output->backend;
	struct weston_compositor *ec = output->base.compositor;
	pixman_region32_t damage;

	assert(output);

	if (output->buf_count == 0) {
		weston_output_arm_frame_timer(output_base,
					      output->finish_frame_timer);
		return 0;
	}

	int idx = get_selected_idx(b->display);
	weston_renderbuffer_t rb = output->renderbuffers[idx];

	pixman_region32_init(&damage);
	weston_output_flush_damage_for_primary_plane(output_base, &damage);

	ec->renderer->repaint_output(&output->base, &damage, rb);
	pixman_region32_fini(&damage);

	if (b->consumer_ready) {
		trigger_refresh(b->display);
		b->consumer_ready = false;
	}

	if (b->refresh == 0) {
		struct wl_event_loop *loop =
			wl_display_get_event_loop(ec->wl_display);
		wl_event_loop_add_idle(loop, finish_frame_idle, output);
	} else {
		weston_output_arm_frame_timer(output_base,
					      output->finish_frame_timer);
	}

	return 0;
}

static void
vdrm_output_disable_renderer(struct vdrm_output *output)
{
	struct weston_renderer *renderer = output->base.compositor->renderer;

	for (int i = 0; i < output->buf_count; i++) {
		if (output->renderbuffers[i]) {
			renderer->destroy_renderbuffer(output->renderbuffers[i]);
			output->renderbuffers[i] = NULL;
		}
	}
	output->buf_count = 0;

	switch (renderer->type) {
	case WESTON_RENDERER_GL:
		renderer->gl->output_destroy(&output->base);
		break;
	case WESTON_RENDERER_VULKAN:
		renderer->vulkan->output_destroy(&output->base);
		break;
	default:
		break;
	}
}

static int
vdrm_output_disable(struct weston_output *base)
{
	struct vdrm_output *output = to_vdrm_output(base);

	assert(output);

	if (!output->base.enabled)
		return 0;

	wl_event_source_remove(output->finish_frame_timer);
	vdrm_output_disable_renderer(output);

	return 0;
}

static void
vdrm_output_destroy(struct weston_output *base)
{
	struct vdrm_output *output = to_vdrm_output(base);

	assert(output);

	vdrm_output_disable(&output->base);
	weston_output_release(&output->base);
	free(output);
}

static int
vdrm_output_enable_renderer(struct vdrm_output *output)
{
	struct vdrm_backend *b = output->backend;
	const struct weston_renderer *renderer = b->compositor->renderer;
	const struct weston_mode *mode = output->base.current_mode;

	switch (renderer->type) {
	case WESTON_RENDERER_GL: {
		struct gl_renderer_fbo_options gl_opts = {
			.fb_size = { .width = mode->width, .height = mode->height },
			.area = { .x = 0, .y = 0,
				  .width = mode->width, .height = mode->height },
		};
		if (renderer->gl->output_fbo_create(&output->base, &gl_opts) < 0) {
			weston_log("virtual-drm: failed to create GL FBO output\n");
			return -1;
		}
		break;
	}
	case WESTON_RENDERER_VULKAN: {
		struct vulkan_renderer_surfaceless_options vk_opts = {
			.fb_size = { .width = mode->width, .height = mode->height },
			.area = { .x = 0, .y = 0,
				  .width = mode->width, .height = mode->height },
		};
		if (renderer->vulkan->output_surfaceless_create(&output->base, &vk_opts) < 0) {
			weston_log("virtual-drm: failed to create Vulkan surfaceless output\n");
			return -1;
		}
		break;
	}
	default:
		weston_log("virtual-drm: unsupported renderer type %d\n", renderer->type);
		return -1;
	}

	return 0;
}

static int
vdrm_output_enable(struct weston_output *base)
{
	struct vdrm_output *output = to_vdrm_output(base);
	struct wl_event_loop *loop;

	assert(output);

	loop = wl_display_get_event_loop(output->backend->compositor->wl_display);
	output->finish_frame_timer =
		wl_event_loop_add_timer(loop, finish_frame_handler, output);

	if (!output->finish_frame_timer) {
		weston_log("virtual-drm: failed to add finish frame timer\n");
		return -1;
	}

	if (vdrm_output_enable_renderer(output) < 0) {
		wl_event_source_remove(output->finish_frame_timer);
		return -1;
	}

	return 0;
}

static int
vdrm_output_set_size(struct weston_output *base, int width, int height)
{
	struct vdrm_output *output = to_vdrm_output(base);
	struct weston_head *head;

	if (!output)
		return -1;

	if (output->backend->screen_w > 0 && output->backend->screen_h > 0) {
		width = output->backend->screen_w;
		height = output->backend->screen_h;
	}

	assert(!output->base.current_mode);
	assert(output->base.current_scale);

	wl_list_for_each(head, &output->base.head_list, output_link) {
		weston_head_set_monitor_strings(head, "virtual-drm", "vdrm",
						NULL);
		weston_head_set_physical_size(head, width, height);
	}

	output->mode.flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	output->mode.width = width * output->base.current_scale;
	output->mode.height = height * output->base.current_scale;
	output->mode.refresh = output->backend->refresh > 0 ?
			       output->backend->refresh : DEFAULT_REFRESH;
	wl_list_insert(&output->base.mode_list, &output->mode.link);

	output->base.current_mode = &output->mode;

	output->base.start_repaint_loop = vdrm_output_start_repaint_loop;
	output->base.repaint = vdrm_output_repaint;
	output->base.assign_planes = NULL;
	output->base.set_backlight = NULL;
	output->base.set_dpms = NULL;
	output->base.switch_mode = NULL;

	return 0;
}

static struct weston_output *
vdrm_output_create(struct weston_backend *backend, const char *name)
{
	struct vdrm_backend *b = to_vdrm_backend(backend);
	struct vdrm_output *output;

	assert(name);

	output = zalloc(sizeof *output);
	if (!output)
		return NULL;

	weston_output_init(&output->base, b->compositor, name);

	output->base.destroy = vdrm_output_destroy;
	output->base.disable = vdrm_output_disable;
	output->base.enable = vdrm_output_enable;
	output->base.attach_head = NULL;

	output->backend = b;

	weston_compositor_add_pending_output(&output->base, b->compositor);

	return &output->base;
}

static int
vdrm_head_create(struct weston_backend *base, const char *name)
{
	struct vdrm_backend *backend = to_vdrm_backend(base);
	struct vdrm_head *head;

	assert(name);

	head = zalloc(sizeof *head);
	if (!head)
		return -1;

	weston_head_init(&head->base, name);
	head->base.backend = &backend->base;
	weston_head_set_connection_status(&head->base, true);
	weston_head_set_supported_eotf_mask(&head->base,
					    WESTON_EOTF_MODE_ALL_MASK);
	weston_head_set_supported_colorimetry_mask(&head->base,
						   WESTON_COLORIMETRY_MODE_ALL_MASK);

	weston_compositor_add_head(backend->compositor, &head->base);

	return 0;
}

static void
vdrm_head_destroy(struct weston_head *base)
{
	struct vdrm_head *head = to_vdrm_head(base);

	assert(head);

	weston_head_release(&head->base);
	free(head);
}

static bool
vdrm_input_init(struct vdrm_backend *b)
{
	weston_seat_init(&b->seat, b->compositor, "default");
	weston_seat_init_pointer(&b->seat);

	if (weston_seat_init_keyboard(&b->seat, NULL) < 0)
		return false;

	weston_seat_init_touch(&b->seat);

	b->touch_device = weston_touch_create_touch_device(
		b->seat.touch_state, "virtual-drm-touch", NULL, NULL, NULL);

	return true;
}

static void
vdrm_input_destroy(struct vdrm_backend *b)
{
	if (b->touch_device) {
		weston_touch_device_destroy(b->touch_device);
		b->touch_device = NULL;
	}
	weston_seat_release(&b->seat);
}

static void
vdrm_destroy(struct weston_backend *backend)
{
	struct vdrm_backend *b = to_vdrm_backend(backend);
	struct weston_compositor *ec = b->compositor;
	struct weston_head *base, *next;

	wl_list_remove(&b->base.link);

	if (b->buf_ready_source)
		wl_event_source_remove(b->buf_ready_source);
	if (b->input_source)
		wl_event_source_remove(b->input_source);

	wl_list_for_each_safe(base, next, &ec->head_list, compositor_link) {
		if (to_vdrm_head(base))
			vdrm_head_destroy(base);
	}

	vdrm_input_destroy(b);

	if (b->display)
		disconnect(b->display);

	free(b->formats);
	free(b->socket_path);
	free(b);
}

#define WESTON_WINDOWED_OUTPUT_API_NAME_VIRTUAL_DRM \
	"weston_windowed_output_api_virtual_drm_v2"

static const struct weston_windowed_output_api api = {
	vdrm_output_set_size,
	vdrm_head_create,
};

static struct vdrm_backend *
vdrm_backend_create(struct weston_compositor *compositor,
		    struct weston_virtual_drm_backend_config *config)
{
	struct vdrm_backend *b;
	int ret;

	b = zalloc(sizeof *b);
	if (!b)
		return NULL;

	b->compositor = compositor;
	wl_list_insert(&compositor->backend_list, &b->base.link);

	b->base.supported_presentation_clocks =
		WESTON_PRESENTATION_CLOCKS_SOFTWARE;

	b->socket_path = strdup(config->socket_path ?
				config->socket_path : DEFAULT_SOCKET_PATH);
	b->refresh = config->refresh > 0 ? config->refresh : DEFAULT_REFRESH;

	b->formats_count = ARRAY_LENGTH(vdrm_formats);
	b->formats = pixel_format_get_array(vdrm_formats, b->formats_count);

	if (!vdrm_input_init(b))
		goto err_free;

	b->base.destroy = vdrm_destroy;
	b->base.create_output = vdrm_output_create;

	if (connect_to_deamon(&b->display, b->socket_path) < 0) {
		weston_log("virtual-drm: failed to connect to daemon at %s\n",
			   b->socket_path);
		goto err_input;
	}

	set_fallback_callback(b->display, vdrm_fallback_cb, b);

	uint32_t sw, sh, sf, sr;
	get_screen_info(b->display, &sw, &sh, &sf, &sr);
	b->refresh = sr;
	b->screen_w = sw;
	b->screen_h = sh;

	weston_log("virtual-drm: connected to daemon at %s, refresh=%d mHz\n",
		   b->socket_path, b->refresh);

	if (config->renderer != WESTON_RENDERER_GL) {
		const struct vulkan_renderer_display_options vk_options = {
			.formats = b->formats,
			.formats_count = b->formats_count,
		};
		ret = weston_compositor_init_renderer(compositor,
						      WESTON_RENDERER_VULKAN,
						      &vk_options.base);
		if (ret == 0)
			goto renderer_ok;
		if (config->renderer == WESTON_RENDERER_VULKAN) {
			weston_log("virtual-drm: Vulkan renderer failed\n");
			goto err_display;
		}
		weston_log("virtual-drm: Vulkan renderer failed, trying GL\n");
	}
	{
		const struct gl_renderer_display_options gl_options = {
			.egl_platform = EGL_PLATFORM_SURFACELESS_MESA,
			.egl_native_display = NULL,
			.formats = b->formats,
			.formats_count = b->formats_count,
		};
		ret = weston_compositor_init_renderer(compositor,
						      WESTON_RENDERER_GL,
						      &gl_options.base);
	}
	if (ret < 0) {
		weston_log("virtual-drm: GL renderer failed\n");
		goto err_display;
	}
renderer_ok:

	struct wl_event_loop *loop =
		wl_display_get_event_loop(compositor->wl_display);
	b->input_source = wl_event_loop_add_timer(loop, vdrm_input_timer_handler, b);
	if (b->input_source)
		wl_event_source_timer_update(b->input_source, 8);

	int buf_ready_fd = get_buffer_ready_fd(b->display);
	if (buf_ready_fd >= 0) {
		b->buf_ready_source = wl_event_loop_add_fd(loop, buf_ready_fd,
							    WL_EVENT_READABLE,
							    vdrm_buf_ready_handler, b);
	}

	ret = weston_plugin_api_register(compositor,
					 WESTON_WINDOWED_OUTPUT_API_NAME_VIRTUAL_DRM,
					 &api, sizeof(api));
	if (ret < 0) {
		weston_log("virtual-drm: failed to register output API\n");
		goto err_display;
	}

	return b;

err_display:
	disconnect(b->display);
	b->display = NULL;
err_input:
	vdrm_input_destroy(b);
err_free:
	wl_list_remove(&b->base.link);
	free(b->formats);
	free(b->socket_path);
	free(b);
	return NULL;
}

WL_EXPORT int
weston_backend_init(struct weston_compositor *compositor,
		    struct weston_backend_config *config_base)
{
	struct weston_virtual_drm_backend_config config = {{ 0, }};

	if (!config_base ||
	    config_base->struct_version != WESTON_VIRTUAL_DRM_BACKEND_CONFIG_VERSION ||
	    config_base->struct_size > sizeof(struct weston_virtual_drm_backend_config)) {
		weston_log("virtual-drm backend config structure is invalid\n");
		return -1;
	}

	memcpy(&config, config_base, config_base->struct_size);

	if (!vdrm_backend_create(compositor, &config))
		return -1;

	return 0;
}
