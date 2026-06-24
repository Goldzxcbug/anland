#ifndef DISPLAY_CONSUMER_H
#define DISPLAY_CONSUMER_H

#include <stdint.h>
#include "../common/protocol.h"

typedef struct display_ctx display_ctx;

int  connect_to_deamon(display_ctx **ctx, const char *socket_path);
/* Like connect_to_deamon, but takes ownership of an already-connected control
 * fd (e.g. one passed in from a root helper) instead of dialing socket_path
 * itself. On success the ctx owns ctrl_fd; on failure ctrl_fd is closed. */
int  connect_to_deamon_with_fd(display_ctx **ctx, int ctrl_fd);
void disconnect(display_ctx *ctx);
int  set_screen_info(display_ctx *ctx, uint32_t width, uint32_t height, uint32_t format, uint32_t refresh);
int  push_dmabufs(display_ctx *ctx, const int *fds, const struct buf_info *infos, int count);
int  select_dmabuf(display_ctx *ctx, int idx);
int  refresh_done(display_ctx *ctx);
int  push_input_event(display_ctx *ctx, const struct InputEvent *event);
int  push_input_event_with_length(display_ctx *ctx, const struct InputEvent *event, void* payload, size_t size);
int  set_fallback_callback(display_ctx *ctx, void (*on_fallback)(void *), void *userdata);
int  poll_output_event(display_ctx *ctx, struct OutputEvent *event, int timeout_ms);
int  poll_output_event_extend_data(display_ctx *ctx, void* payload, size_t size, int timeout_ms);
int  set_exit_fallback_callback(display_ctx *ctx, void (*on_exit_fallback)(void *), void *userdata);
int  get_data_fd(display_ctx *ctx);
void handle_unhandled_event(display_ctx *ctx, const struct OutputEvent *event);

#endif
