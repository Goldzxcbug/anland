/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#include "config.h"

#include "backends/anland/meta-backend-anland.h"

#include "backends/anland/meta-anland-audio.h"
#include "backends/anland/meta-anland-camera.h"
#include "backends/anland/meta-anland-clipboard.h"
#include "backends/anland/meta-anland-input.h"
#include "backends/anland/vendor/meta-anland-transport.h"
#include "backends/meta-backend-private.h"
#include "backends/meta-fd-source.h"
#include "backends/meta-monitor-manager-private.h"
#include "backends/meta-renderer.h"
#include "backends/meta-renderer-view.h"
#include "backends/meta-stage-view-private.h"
#include "backends/meta-virtual-monitor.h"
#include "backends/native/meta-renderer-native-private.h"
#include "meta/meta-backend.h"
#include "meta/meta-context.h"
#include "meta/meta-wayland-compositor.h"
#include "wayland/meta-wayland.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#define ANLAND_RECONNECT_INTERVAL_MS 200
#define ANLAND_PROTOCOL_FORMAT_RGBA_8888 1
#define ANLAND_MAX_RESOURCE_FDS 64

enum
{
  PROP_0,

  PROP_ANLAND_SOCKET,
};

struct _MetaBackendAnland
{
  MetaBackendNative parent;

  char *socket_path;
  MetaAnlandTransport *transport;
  MetaVirtualMonitor *virtual_monitor;
  struct screen_info screen_info;
  guint reconnect_source_id;
  MetaAnlandAudio *audio;
  MetaAnlandCamera *camera;
  MetaAnlandInput *input;
  MetaAnlandClipboard *clipboard;

  GSource *buffer_ready_source;
  GSource *input_source;
  MetaStageView *stage_view;
  CoglFramebuffer *placeholder_framebuffer;
  CoglFramebuffer *buffer_framebuffers[MAX_BUFS];
  int current_buffer;
  gboolean consumer_active;
  gboolean frame_clock_inhibited;
  gboolean frame_pending;
  gboolean warned_sync_fallback;
  int64_t pending_global_frame_counter;
  int64_t pending_view_frame_counter;
  unsigned int presentation_sequence;
};

G_DEFINE_TYPE (MetaBackendAnland, meta_backend_anland, META_TYPE_BACKEND_NATIVE)

static void deactivate_consumer (MetaBackendAnland *backend);
static void release_stage_view (MetaBackendAnland *backend);
static void schedule_reconnect (MetaBackendAnland *backend);

static void
on_context_started (MetaContext        *context,
                    MetaBackendAnland *backend)
{
  backend->clipboard = meta_anland_clipboard_new (META_BACKEND (backend));
  if (!backend->clipboard)
    g_warning ("Failed to initialize Anland clipboard bridge");
}

static gboolean
read_text_payload (MetaBackendAnland  *backend,
                   uint32_t            size,
                   const char         *name,
                   GBytes            **contents)
{
  g_autofree char *text = NULL;

  *contents = NULL;
  if (size > META_ANLAND_MAX_PAYLOAD_SIZE)
    {
      g_warning ("Anland rejected oversized %s payload", name);
      return FALSE;
    }

  text = g_malloc (size + 1);
  if (meta_anland_transport_read_input_payload (backend->transport, text,
                                                size, 100) != 1)
    return FALSE;
  text[size] = '\0';

  if (memchr (text, '\0', size) || !g_utf8_validate (text, size, NULL))
    {
      g_warning ("Anland ignored invalid UTF-8 %s payload", name);
      return TRUE;
    }

  *contents = g_bytes_new (text, size);
  return TRUE;
}

static gboolean
handle_extended_input (MetaBackendAnland       *backend,
                       const struct InputEvent *event)
{
  g_autoptr (GBytes) contents = NULL;
  g_autofree char *input_text = NULL;
  const char *text;
  guint size;

  switch (event->type)
    {
    case INPUT_TYPE_CLIPBOARD:
      if (!read_text_payload (backend, event->clipboard.size, "clipboard",
                              &contents))
        return FALSE;
      break;
    case INPUT_TYPE_TEXT_INPUT:
      if (!read_text_payload (backend, event->text_input.size, "text input",
                              &contents))
        return FALSE;
      break;
    default:
      return FALSE;
    }

  if (!contents)
    return TRUE;

  size = g_bytes_get_size (contents);
  if (size == 0)
    return TRUE;

  text = g_bytes_get_data (contents, NULL);
  if (event->type == INPUT_TYPE_CLIPBOARD)
    return meta_anland_clipboard_set_from_consumer (backend->clipboard,
                                                    contents);

  input_text = g_strndup (text, size);

  {
    MetaContext *context = meta_backend_get_context (META_BACKEND (backend));
    MetaWaylandCompositor *compositor =
      meta_context_get_wayland_compositor (context);

    if (compositor && meta_wayland_text_input_commit_string (
          meta_wayland_compositor_get_text_input (compositor), input_text))
      return TRUE;
  }

  return meta_anland_input_inject_text (backend->input, input_text);
}

static void
close_resource_fds (int *fds,
                    int  n_fds)
{
  int i;

  for (i = 0; i < n_fds; i++)
    {
      if (fds[i] >= 0)
        close (fds[i]);
    }
}

static gboolean
set_resource_fds_cloexec (int *fds,
                          int  n_fds)
{
  int i;

  for (i = 0; i < n_fds; i++)
    {
      int flags = fcntl (fds[i], F_GETFD);

      if (flags < 0 || fcntl (fds[i], F_SETFD, flags | FD_CLOEXEC) < 0)
        return FALSE;
    }

  return TRUE;
}

static gboolean
handle_resource_input (MetaBackendAnland       *backend,
                       const struct InputEvent *event)
{
  int fds[ANLAND_MAX_RESOURCE_FDS];
  int n_fds = 0;

  if (event->resource.fdnum == 0 ||
      event->resource.fdnum > G_N_ELEMENTS (fds) ||
      meta_anland_transport_read_input_fds (backend->transport, fds,
                                            G_N_ELEMENTS (fds), &n_fds, 100) != 1 ||
      n_fds != (int) event->resource.fdnum)
    {
      close_resource_fds (fds, n_fds);
      return FALSE;
    }

  if (!set_resource_fds_cloexec (fds, n_fds))
    {
      close_resource_fds (fds, n_fds);
      return FALSE;
    }

  if (event->resource.type == SERVICE_TYPE_CAMERA && backend->camera)
    {
      meta_anland_camera_set_resources (backend->camera, fds[0], &fds[1],
                                        n_fds - 1);
      return TRUE;
    }

  close_resource_fds (fds, n_fds);
  return TRUE;
}

static void
request_camera_resources (MetaBackendAnland *backend)
{
  struct OutputEvent event = {
    .type = OUTPUT_TYPE_RESOURCES_REQUEST,
    .resources_request.type = SERVICE_TYPE_CAMERA,
  };

  if (!backend->camera)
    return;

  meta_anland_transport_send_output_event (backend->transport, &event,
                                           NULL, 0);
}

static char *
get_default_socket_path (void)
{
  const char *socket_path = g_getenv ("ANLAND_SOCKET");
  const char *tmpdir;
  const char *prefix;

  if (socket_path && *socket_path)
    return g_strdup (socket_path);

  tmpdir = g_getenv ("TMPDIR");
  if (tmpdir && *tmpdir)
    return g_build_filename (tmpdir, "anland", "display_daemon.sock", NULL);

  prefix = g_getenv ("PREFIX");
  if (prefix && g_str_has_prefix (prefix, "/data/data/com.termux/"))
    return g_strdup ("/data/data/com.termux/files/usr/tmp/anland/display_daemon.sock");

  return g_strdup ("/tmp/anland/display_daemon.sock");
}

static gboolean
validate_screen_info (const struct screen_info  *screen_info,
                      GError                   **error)
{
  if (screen_info->width == 0 || screen_info->height == 0 ||
      screen_info->width > G_MAXINT || screen_info->height > G_MAXINT ||
      (screen_info->refresh != 0 && screen_info->refresh < 1000) ||
      screen_info->refresh > 1000000)
    {
      if (error)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "Anland daemon provided invalid screen information");
      return FALSE;
    }

  return TRUE;
}

static void
clear_transport (MetaBackendAnland *backend)
{
  deactivate_consumer (backend);

  if (backend->transport)
    meta_anland_transport_disconnect (backend->transport);
  backend->transport = NULL;
}

static gboolean
connect_transport (MetaBackendAnland  *backend,
                   GError            **error)
{
  struct screen_info screen_info;

  if (backend->transport)
    return TRUE;

  if (meta_anland_transport_connect (&backend->transport,
                                     backend->socket_path) < 0)
    {
      if (error)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED,
                     "Failed to connect to Anland daemon at %s",
                     backend->socket_path);
      return FALSE;
    }

  if (!meta_anland_transport_get_screen_info (backend->transport, &screen_info) ||
      !validate_screen_info (&screen_info, error))
    {
      clear_transport (backend);
      return FALSE;
    }

  backend->screen_info = screen_info;
  return TRUE;
}

static gboolean
update_virtual_monitor (MetaBackendAnland  *backend,
                        GError            **error)
{
  MetaMonitorManager *monitor_manager;
  float refresh_rate;
  g_autolist (MetaVirtualModeInfo) mode_infos = NULL;

  if (!backend->transport)
    return FALSE;

  monitor_manager = meta_backend_get_monitor_manager (META_BACKEND (backend));
  refresh_rate = backend->screen_info.refresh > 0 ?
    backend->screen_info.refresh / 1000.0f : 60.0f;
  mode_infos = g_list_append (mode_infos,
                              meta_virtual_mode_info_new (
                                backend->screen_info.width,
                                backend->screen_info.height,
                                refresh_rate));

  if (backend->virtual_monitor)
    {
      release_stage_view (backend);
      meta_virtual_monitor_set_modes (backend->virtual_monitor, mode_infos);
      meta_monitor_manager_reload (monitor_manager);
      return TRUE;
    }

  g_autoptr (MetaVirtualMonitorInfo) info =
    meta_virtual_monitor_info_new ("Anland", "Anland-1", "Anland-1",
                                   mode_infos);
  backend->virtual_monitor =
    meta_monitor_manager_create_virtual_monitor (monitor_manager, info, error);
  if (!backend->virtual_monitor)
    return FALSE;

  meta_monitor_manager_reload (monitor_manager);
  return TRUE;
}

static void
inhibit_frame_clock (MetaBackendAnland *backend)
{
  if (!backend->stage_view || backend->frame_clock_inhibited)
    return;

  clutter_frame_clock_inhibit (
    clutter_stage_view_get_frame_clock (CLUTTER_STAGE_VIEW (backend->stage_view)));
  backend->frame_clock_inhibited = TRUE;
}

static void
uninhibit_frame_clock (MetaBackendAnland *backend)
{
  if (!backend->stage_view || !backend->frame_clock_inhibited)
    return;

  clutter_frame_clock_uninhibit (
    clutter_stage_view_get_frame_clock (CLUTTER_STAGE_VIEW (backend->stage_view)));
  backend->frame_clock_inhibited = FALSE;
}

static void
release_stage_view (MetaBackendAnland *backend)
{
  if (!backend->stage_view)
    return;

  g_return_if_fail (!backend->consumer_active);
  g_return_if_fail (!backend->frame_pending);

  uninhibit_frame_clock (backend);
  g_clear_object (&backend->placeholder_framebuffer);
  g_clear_object (&backend->stage_view);
}

static MetaStageView *
find_stage_view (MetaBackendAnland *backend)
{
  MetaRenderer *renderer;
  MetaCrtc *crtc;
  MetaRendererView *renderer_view;

  if (!backend->virtual_monitor)
    return NULL;

  renderer = meta_backend_get_renderer (META_BACKEND (backend));
  crtc = meta_virtual_monitor_get_crtc (backend->virtual_monitor);
  renderer_view = meta_renderer_get_view_for_crtc (renderer, crtc);

  return renderer_view ? META_STAGE_VIEW (renderer_view) : NULL;
}

static gboolean
ensure_stage_view (MetaBackendAnland  *backend,
                   GError            **error)
{
  MetaStageView *stage_view = find_stage_view (backend);

  if (!stage_view)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Anland virtual monitor does not have a renderer view yet");
      return FALSE;
    }

  if (backend->stage_view == stage_view)
    return TRUE;

  if (backend->consumer_active || backend->frame_pending)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                   "Anland renderer view changed while a consumer was active");
      return FALSE;
    }

  if (backend->frame_clock_inhibited)
    uninhibit_frame_clock (backend);

  g_set_object (&backend->stage_view, stage_view);
  g_set_object (&backend->placeholder_framebuffer,
                clutter_stage_view_get_onscreen (CLUTTER_STAGE_VIEW (stage_view)));
  return TRUE;
}

static gboolean
validate_buffer_info (const struct buf_info  *buffer_info,
                      int                     fd,
                      GError                **error)
{
  struct stat stat_buf;
  uint64_t required_size;

  if (buffer_info->format != ANLAND_PROTOCOL_FORMAT_RGBA_8888 ||
      buffer_info->modifier != DRM_FORMAT_MOD_LINEAR ||
      buffer_info->width > G_MAXUINT32 / 4u ||
      buffer_info->stride < buffer_info->width * 4u ||
      buffer_info->height > G_MAXUINT64 / buffer_info->stride ||
      buffer_info->offset > G_MAXUINT64 -
        (uint64_t) buffer_info->stride * buffer_info->height)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Anland daemon provided an invalid dma-buf description");
      return FALSE;
    }

  required_size = buffer_info->offset +
    (uint64_t) buffer_info->stride * buffer_info->height;
  if (fstat (fd, &stat_buf) == 0 && stat_buf.st_size > 0 &&
      required_size > (uint64_t) stat_buf.st_size)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Anland dma-buf is smaller than its advertised layout");
      return FALSE;
    }

  return TRUE;
}

static void
complete_pending_frame (MetaBackendAnland *backend)
{
  ClutterFrameInfo frame_info;

  if (!backend->stage_view || !backend->frame_pending)
    return;

  frame_info = (ClutterFrameInfo) {
    .global_frame_counter = backend->pending_global_frame_counter,
    .view_frame_counter = backend->pending_view_frame_counter,
    .refresh_rate = clutter_stage_view_get_refresh_rate (
      CLUTTER_STAGE_VIEW (backend->stage_view)),
    .presentation_time = g_get_monotonic_time (),
    .flags = CLUTTER_FRAME_INFO_FLAG_NONE,
    .sequence = ++backend->presentation_sequence,
  };
  clutter_stage_view_notify_presented (CLUTTER_STAGE_VIEW (backend->stage_view),
                                       &frame_info);
  backend->frame_pending = FALSE;
}

static void
deactivate_consumer (MetaBackendAnland *backend)
{
  meta_anland_audio_set_fd (backend->audio, -1);
  meta_anland_camera_clear (backend->camera);
  meta_anland_clipboard_set_transport (backend->clipboard, NULL);

  if (backend->input_source)
    {
      g_source_destroy (backend->input_source);
      g_clear_pointer (&backend->input_source, g_source_unref);
    }

  if (backend->input)
    meta_anland_input_reset (backend->input);

  if (backend->buffer_ready_source)
    {
      g_source_destroy (backend->buffer_ready_source);
      g_clear_pointer (&backend->buffer_ready_source, g_source_unref);
    }

  if (backend->stage_view)
    {
      clutter_stage_view_set_present_func (
        CLUTTER_STAGE_VIEW (backend->stage_view), NULL, NULL);

      if (backend->current_buffer >= 0 &&
          backend->placeholder_framebuffer &&
          clutter_stage_view_get_onscreen (CLUTTER_STAGE_VIEW (backend->stage_view)) ==
            backend->buffer_framebuffers[backend->current_buffer])
        clutter_stage_view_replace_framebuffer (
          CLUTTER_STAGE_VIEW (backend->stage_view),
          backend->placeholder_framebuffer);

      if (backend->frame_pending)
        clutter_stage_view_notify_ready (CLUTTER_STAGE_VIEW (backend->stage_view));
    }

  for (int i = 0; i < MAX_BUFS; i++)
    g_clear_object (&backend->buffer_framebuffers[i]);

  backend->consumer_active = FALSE;
  backend->current_buffer = -1;
  backend->frame_pending = FALSE;
  inhibit_frame_clock (backend);
}

static gboolean
on_stage_view_present (ClutterStageView *stage_view,
                       ClutterFrame     *frame,
                       int64_t           global_frame_counter,
                       gpointer          user_data)
{
  MetaBackendAnland *backend = user_data;
  CoglFramebuffer *framebuffer;
  CoglContext *cogl_context;
  int fence_fd = -1;

  if (!backend->consumer_active || !backend->transport ||
      meta_anland_transport_is_fallback (backend->transport))
    return FALSE;

  framebuffer = clutter_stage_view_get_onscreen (stage_view);
  cogl_context = cogl_framebuffer_get_context (framebuffer);
  cogl_framebuffer_flush (framebuffer);

  if (cogl_context_has_winsys_feature (cogl_context, COGL_WINSYS_FEATURE_SYNC_FD))
    fence_fd = cogl_context_get_latest_sync_fd (cogl_context);
  else
    {
      if (!backend->warned_sync_fallback)
        {
          g_warning ("Anland backend has no native fence support; using synchronous rendering");
          backend->warned_sync_fallback = TRUE;
        }
      cogl_framebuffer_finish (framebuffer);
    }

  meta_anland_transport_set_render_fence (backend->transport, fence_fd);
  if (!meta_anland_transport_notify_frame_done (backend->transport))
    {
      clutter_frame_set_result (frame, CLUTTER_FRAME_RESULT_IDLE);
      return TRUE;
    }

  backend->pending_global_frame_counter = global_frame_counter;
  backend->pending_view_frame_counter = clutter_frame_get_count (frame);
  backend->frame_pending = TRUE;
  inhibit_frame_clock (backend);
  clutter_frame_set_result (frame, CLUTTER_FRAME_RESULT_PENDING_PRESENTED);
  return TRUE;
}

static gboolean
input_source_prepare (gpointer user_data)
{
  return FALSE;
}

static gboolean
input_source_dispatch (gpointer user_data)
{
  MetaBackendAnland *backend = META_BACKEND_ANLAND (user_data);
  struct InputEvent event;
  int result;

  if (!backend->consumer_active || !backend->transport || !backend->input ||
      meta_anland_transport_is_fallback (backend->transport))
    return G_SOURCE_REMOVE;

  while ((result = meta_anland_transport_poll_input_event (
             backend->transport, &event, 0)) > 0)
    {
      MetaAnlandInputEventResult input_result;

      input_result = meta_anland_input_handle_event (backend->input,
                                                      backend->transport,
                                                      &event,
                                                      backend->screen_info.width,
                                                      backend->screen_info.height);
      if (input_result == META_ANLAND_INPUT_EVENT_NEEDS_PAYLOAD)
        {
          if (!handle_extended_input (backend, &event))
            {
              meta_anland_transport_enter_fallback (backend->transport);
              return G_SOURCE_REMOVE;
            }
        }
      else if (input_result == META_ANLAND_INPUT_EVENT_NEEDS_RESOURCE_FDS)
        {
          if (!handle_resource_input (backend, &event))
            {
              meta_anland_transport_enter_fallback (backend->transport);
              return G_SOURCE_REMOVE;
            }
        }
      else if (input_result == META_ANLAND_INPUT_EVENT_ERROR)
        {
          meta_anland_transport_enter_fallback (backend->transport);
          return G_SOURCE_REMOVE;
        }

      if (!backend->consumer_active)
        return G_SOURCE_REMOVE;
    }

  return result < 0 ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

static gboolean
buffer_ready_source_prepare (gpointer user_data)
{
  return FALSE;
}

static gboolean
buffer_ready_source_dispatch (gpointer user_data)
{
  MetaBackendAnland *backend = META_BACKEND_ANLAND (user_data);
  eventfd_t count;
  int buffer_ready_fd;
  int selected_buffer;

  if (!backend->consumer_active || !backend->transport ||
      meta_anland_transport_is_fallback (backend->transport))
    return G_SOURCE_REMOVE;

  buffer_ready_fd = meta_anland_transport_get_buffer_ready_fd (backend->transport);
  if (buffer_ready_fd < 0 || eventfd_read (buffer_ready_fd, &count) < 0 ||
      count != 1)
    {
      meta_anland_transport_enter_fallback (backend->transport);
      return G_SOURCE_REMOVE;
    }

  selected_buffer = meta_anland_transport_get_selected_buffer (backend->transport);
  if (selected_buffer < 0 || selected_buffer >= MAX_BUFS ||
      !backend->buffer_framebuffers[selected_buffer])
    {
      meta_anland_transport_enter_fallback (backend->transport);
      return G_SOURCE_REMOVE;
    }

  complete_pending_frame (backend);

  if (backend->current_buffer != selected_buffer)
    {
      clutter_stage_view_replace_framebuffer (
        CLUTTER_STAGE_VIEW (backend->stage_view),
        backend->buffer_framebuffers[selected_buffer]);
      backend->current_buffer = selected_buffer;
    }

  clutter_stage_view_add_redraw_clip (CLUTTER_STAGE_VIEW (backend->stage_view),
                                      NULL);
  uninhibit_frame_clock (backend);
  clutter_stage_view_schedule_update_now (CLUTTER_STAGE_VIEW (backend->stage_view));
  return G_SOURCE_CONTINUE;
}

static gboolean
activate_consumer (MetaBackendAnland *backend)
{
  MetaRenderer *renderer = meta_backend_get_renderer (META_BACKEND (backend));
  MetaRendererNative *renderer_native = META_RENDERER_NATIVE (renderer);
  CoglFramebuffer *framebuffers[MAX_BUFS] = { NULL, };
  struct buf_info buffer_info;
  int n_buffers;
  int buffer_ready_fd;
  int buffer_ready_source_fd = -1;
  int input_fd;
  int input_source_fd = -1;
  int i;
  g_autoptr (GError) error = NULL;

  n_buffers = meta_anland_transport_get_buffer_count (backend->transport);
  if (n_buffers < 1 || n_buffers > MAX_BUFS ||
      !meta_anland_transport_get_buffer_info (backend->transport, 0,
                                              &buffer_info))
    goto failed;

  if (buffer_info.width != backend->screen_info.width ||
      buffer_info.height != backend->screen_info.height)
    {
      backend->screen_info.width = buffer_info.width;
      backend->screen_info.height = buffer_info.height;
      if (!update_virtual_monitor (backend, &error))
        g_warning ("Failed to update the Anland virtual monitor: %s",
                   error->message);
      return FALSE;
    }

  if (!ensure_stage_view (backend, &error))
    return FALSE;

  if (!backend->clipboard)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Anland clipboard bridge is unavailable");
      goto failed;
    }

  if (cogl_framebuffer_get_width (backend->placeholder_framebuffer) !=
        (int) buffer_info.width ||
      cogl_framebuffer_get_height (backend->placeholder_framebuffer) !=
        (int) buffer_info.height)
    return FALSE;

  if (backend->screen_info.refresh > 0 &&
      fabsf (clutter_stage_view_get_refresh_rate (
               CLUTTER_STAGE_VIEW (backend->stage_view)) -
             backend->screen_info.refresh / 1000.0f) > 0.01f)
    return FALSE;

  for (i = 0; i < n_buffers; i++)
    {
      int fd;
      uint32_t stride;
      uint32_t offset;
      uint64_t modifier;

      if (!meta_anland_transport_get_buffer_info (backend->transport, i,
                                                  &buffer_info))
        goto failed;

      fd = meta_anland_transport_get_buffer_fd (backend->transport, i);
      if (fd < 0 || !validate_buffer_info (&buffer_info, fd, &error))
        goto failed;

      stride = buffer_info.stride;
      offset = buffer_info.offset;
      modifier = buffer_info.modifier;
      framebuffers[i] = meta_renderer_native_create_dma_buf_framebuffer (
        renderer_native, buffer_info.width, buffer_info.height,
        DRM_FORMAT_ABGR8888, 1, &fd, &stride, &offset, &modifier, &error);
      if (!framebuffers[i])
        {
          g_prefix_error (&error,
                          "Failed to import Anland buffer %d "
                          "(%ux%u, stride %u, offset %u, "
                          "modifier %" G_GUINT64_FORMAT "): ",
                          i, buffer_info.width, buffer_info.height,
                          buffer_info.stride, buffer_info.offset,
                          buffer_info.modifier);
          goto failed;
        }
    }

  buffer_ready_fd = meta_anland_transport_get_buffer_ready_fd (backend->transport);
  buffer_ready_source_fd = fcntl (buffer_ready_fd, F_DUPFD_CLOEXEC, 3);
  if (buffer_ready_source_fd < 0)
    {
      g_set_error (&error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Failed to duplicate Anland buffer-ready eventfd");
      goto failed;
    }

  input_fd = meta_anland_transport_get_data_fd (backend->transport);
  input_source_fd = fcntl (input_fd, F_DUPFD_CLOEXEC, 3);
  if (input_source_fd < 0)
    {
      g_set_error (&error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Failed to duplicate Anland data fd");
      goto failed;
    }

  for (i = 0; i < n_buffers; i++)
    backend->buffer_framebuffers[i] = framebuffers[i];
  backend->consumer_active = TRUE;
  meta_anland_clipboard_set_transport (backend->clipboard, backend->transport);
  backend->current_buffer = -1;
  inhibit_frame_clock (backend);
  clutter_stage_view_set_present_func (CLUTTER_STAGE_VIEW (backend->stage_view),
                                       on_stage_view_present, backend);
  backend->buffer_ready_source = meta_create_fd_source (
    buffer_ready_source_fd, "[mutter] Anland buffer ready",
    buffer_ready_source_prepare, buffer_ready_source_dispatch, backend, NULL);
  g_source_attach (backend->buffer_ready_source, NULL);
  backend->input_source = meta_create_fd_source (
    input_source_fd, "[mutter] Anland input",
    input_source_prepare, input_source_dispatch, backend, NULL);
  g_source_attach (backend->input_source, NULL);
  meta_anland_audio_set_fd (backend->audio,
                            meta_anland_transport_get_audio_fd (
                              backend->transport));
  request_camera_resources (backend);
  return TRUE;

failed:
  g_warning ("Failed to activate Anland consumer buffers: %s",
             error ? error->message : "invalid transport state");
  for (i = 0; i < MAX_BUFS; i++)
    g_clear_object (&framebuffers[i]);
  if (buffer_ready_source_fd >= 0)
    close (buffer_ready_source_fd);
  if (input_source_fd >= 0)
    close (input_source_fd);
  meta_anland_transport_enter_fallback (backend->transport);
  return FALSE;
}

static gboolean reconnect_cb (gpointer user_data);

static void
schedule_reconnect (MetaBackendAnland *backend)
{
  if (backend->reconnect_source_id)
    return;

  backend->reconnect_source_id =
    g_timeout_add (ANLAND_RECONNECT_INTERVAL_MS, reconnect_cb, backend);
}

static void
on_transport_fallback (void *user_data)
{
  MetaBackendAnland *backend = user_data;

  deactivate_consumer (backend);
  schedule_reconnect (backend);
}

static void
on_input_display_refresh (uint32_t refresh_mhz,
                          gpointer user_data)
{
  MetaBackendAnland *backend = META_BACKEND_ANLAND (user_data);

  if (backend->screen_info.refresh == refresh_mhz)
    return;

  backend->screen_info.refresh = refresh_mhz;
  deactivate_consumer (backend);
  if (!update_virtual_monitor (backend, NULL))
    {
      clear_transport (backend);
      return;
    }

  schedule_reconnect (backend);
}

static gboolean
reconnect_cb (gpointer user_data)
{
  MetaBackendAnland *backend = META_BACKEND_ANLAND (user_data);
  MetaAnlandTransportPickupResult pickup_result;

  if (!backend->transport)
    {
      if (!connect_transport (backend, NULL))
        return G_SOURCE_CONTINUE;
      if (!update_virtual_monitor (backend, NULL))
        {
          clear_transport (backend);
          return G_SOURCE_CONTINUE;
        }
      meta_anland_transport_set_fallback_callback (backend->transport,
                                                    on_transport_fallback,
                                                    backend);
    }

  pickup_result = meta_anland_transport_try_pickup (backend->transport);
  switch (pickup_result)
    {
    case META_ANLAND_TRANSPORT_PICKUP_READY:
      if (!activate_consumer (backend))
        return G_SOURCE_CONTINUE;
      backend->reconnect_source_id = 0;
      return G_SOURCE_REMOVE;
    case META_ANLAND_TRANSPORT_PICKUP_NO_CONSUMER:
      return G_SOURCE_CONTINUE;
    case META_ANLAND_TRANSPORT_PICKUP_DAEMON_LOST:
    case META_ANLAND_TRANSPORT_PICKUP_PROTOCOL_ERROR:
      clear_transport (backend);
      return G_SOURCE_CONTINUE;
    }

  g_assert_not_reached ();
}

static gboolean
meta_backend_anland_init_post (MetaBackend  *backend,
                               GError      **error)
{
  MetaBackendClass *parent_class =
    META_BACKEND_CLASS (meta_backend_anland_parent_class);
  MetaBackendAnland *backend_anland = META_BACKEND_ANLAND (backend);

  if (!parent_class->init_post (backend, error))
    return FALSE;

  backend_anland->input = meta_anland_input_new (
    backend, on_input_display_refresh, backend_anland, error);
  if (!backend_anland->input)
    return FALSE;

  backend_anland->audio = meta_anland_audio_new ();
  if (!backend_anland->audio)
    g_warning ("Failed to initialize Anland audio bridge");

  backend_anland->camera = meta_anland_camera_new ();
  if (!backend_anland->camera)
    g_warning ("Failed to initialize Anland camera bridge");

  g_signal_connect_object (meta_backend_get_context (backend), "started",
                           G_CALLBACK (on_context_started), backend, 0);

  if (!connect_transport (backend_anland, error))
    return FALSE;

  meta_anland_transport_set_fallback_callback (backend_anland->transport,
                                                on_transport_fallback,
                                                backend_anland);
  return TRUE;
}

static void
meta_backend_anland_update_stage (MetaBackend *backend)
{
  MetaBackendAnland *backend_anland = META_BACKEND_ANLAND (backend);
  MetaBackendClass *parent_class =
    META_BACKEND_CLASS (meta_backend_anland_parent_class);
  gboolean had_stage_view = backend_anland->stage_view != NULL;

  if (backend_anland->consumer_active)
    deactivate_consumer (backend_anland);
  release_stage_view (backend_anland);

  parent_class->update_stage (backend);

  if (had_stage_view && backend_anland->transport)
    schedule_reconnect (backend_anland);
}

static void
meta_backend_anland_set_property (GObject      *object,
                                  guint         prop_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
  MetaBackendAnland *backend = META_BACKEND_ANLAND (object);

  switch (prop_id)
    {
    case PROP_ANLAND_SOCKET:
      if (g_value_get_string (value))
        {
          g_free (backend->socket_path);
          backend->socket_path = g_value_dup_string (value);
        }
      break;
    default:
      G_OBJECT_CLASS (meta_backend_anland_parent_class)->set_property (object,
                                                                        prop_id,
                                                                        value,
                                                                        pspec);
      break;
    }
}

static void
meta_backend_anland_dispose (GObject *object)
{
  MetaBackendAnland *backend = META_BACKEND_ANLAND (object);

  g_clear_handle_id (&backend->reconnect_source_id, g_source_remove);
  clear_transport (backend);
  uninhibit_frame_clock (backend);
  g_clear_pointer (&backend->audio, meta_anland_audio_free);
  g_clear_pointer (&backend->camera, meta_anland_camera_free);
  g_clear_pointer (&backend->clipboard, meta_anland_clipboard_free);
  g_clear_pointer (&backend->input, meta_anland_input_free);
  g_clear_object (&backend->placeholder_framebuffer);
  g_clear_object (&backend->stage_view);
  g_clear_object (&backend->virtual_monitor);
  g_clear_pointer (&backend->socket_path, g_free);

  G_OBJECT_CLASS (meta_backend_anland_parent_class)->dispose (object);
}

static void
meta_backend_anland_class_init (MetaBackendAnlandClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  MetaBackendClass *backend_class = META_BACKEND_CLASS (klass);

  object_class->set_property = meta_backend_anland_set_property;
  object_class->dispose = meta_backend_anland_dispose;
  backend_class->init_post = meta_backend_anland_init_post;
  backend_class->update_stage = meta_backend_anland_update_stage;

  g_object_class_install_property (
    object_class, PROP_ANLAND_SOCKET,
    g_param_spec_string ("anland-socket", NULL, NULL, NULL,
                         G_PARAM_WRITABLE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS));
}

static void
meta_backend_anland_init (MetaBackendAnland *backend_anland)
{
  backend_anland->socket_path = get_default_socket_path ();
  backend_anland->current_buffer = -1;
}

gboolean
meta_backend_anland_setup (MetaBackendAnland  *backend,
                           GError            **error)
{
  if (!update_virtual_monitor (backend, error))
    return FALSE;

  schedule_reconnect (backend);
  return TRUE;
}
