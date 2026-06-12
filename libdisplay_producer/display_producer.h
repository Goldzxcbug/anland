#ifndef DISPLAY_PRODUCER_H
#define DISPLAY_PRODUCER_H

#include <stdbool.h>
#include <stdint.h>
#include "../common/protocol.h"

typedef struct display_ctx display_ctx;

int  connect_to_deamon(display_ctx **ctx, const char *socket_path);
void disconnect(display_ctx *ctx);
int  get_screen_info(display_ctx *ctx, uint32_t *width, uint32_t *height, uint32_t *format, uint32_t *refresh);
int  wait_buffer_async(display_ctx *ctx);
int  wait_buffer_async_result(display_ctx *ctx, void **buffer);
int  trigger_refresh(display_ctx *ctx);
int  poll_input_event(display_ctx *ctx, struct InputEvent *event, int timeout_ms);
int  set_fallback_callback(display_ctx *ctx, void (*on_fallback)(void *), void *userdata);
bool is_fallback(display_ctx *ctx);
int  try_reconnect(display_ctx *ctx);
int  get_buffer_ready_fd(display_ctx *ctx);
int  get_buf_count(display_ctx *ctx);
int  get_selected_idx(display_ctx *ctx);
int  get_dmabuf_fd(display_ctx *ctx);
int  get_dmabuf_fd_at(display_ctx *ctx, int idx);
int  get_dmabuf_info(display_ctx *ctx, struct buf_info *info);
int  get_dmabuf_info_at(display_ctx *ctx, int idx, struct buf_info *info);

#endif
