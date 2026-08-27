#ifndef META_ANLAND_TRANSPORT_H
#define META_ANLAND_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "meta-anland-protocol.h"

typedef struct _MetaAnlandTransport MetaAnlandTransport;

#define META_ANLAND_MAX_PAYLOAD_SIZE (1024 * 1024)

typedef enum
{
  META_ANLAND_TRANSPORT_PICKUP_READY,
  META_ANLAND_TRANSPORT_PICKUP_NO_CONSUMER,
  META_ANLAND_TRANSPORT_PICKUP_DAEMON_LOST = -1,
  META_ANLAND_TRANSPORT_PICKUP_PROTOCOL_ERROR = -2,
} MetaAnlandTransportPickupResult;

int meta_anland_transport_connect (MetaAnlandTransport **transport,
                                   const char           *socket_path);
void meta_anland_transport_disconnect (MetaAnlandTransport *transport);

bool meta_anland_transport_get_screen_info (MetaAnlandTransport *transport,
                                            struct screen_info   *screen_info);

MetaAnlandTransportPickupResult
meta_anland_transport_try_pickup (MetaAnlandTransport *transport);

bool meta_anland_transport_is_fallback (MetaAnlandTransport *transport);
void meta_anland_transport_enter_fallback (MetaAnlandTransport *transport);
void meta_anland_transport_set_fallback_callback (MetaAnlandTransport *transport,
                                                  void                 (*callback) (void *user_data),
                                                  void                  *user_data);

int meta_anland_transport_get_buffer_ready_fd (MetaAnlandTransport *transport);
int meta_anland_transport_get_data_fd (MetaAnlandTransport *transport);
int meta_anland_transport_get_audio_fd (MetaAnlandTransport *transport);
int meta_anland_transport_get_buffer_count (MetaAnlandTransport *transport);
int meta_anland_transport_get_selected_buffer (MetaAnlandTransport *transport);
int meta_anland_transport_get_buffer_fd (MetaAnlandTransport *transport,
                                         int                   index);
bool meta_anland_transport_get_buffer_info (MetaAnlandTransport *transport,
                                            int                   index,
                                            struct buf_info      *buffer_info);

void meta_anland_transport_set_render_fence (MetaAnlandTransport *transport,
                                             int                   fence_fd);
bool meta_anland_transport_notify_frame_done (MetaAnlandTransport *transport);

int meta_anland_transport_poll_input_event (MetaAnlandTransport *transport,
                                            struct InputEvent    *event,
                                            int                   timeout_ms);
int meta_anland_transport_read_input_payload (MetaAnlandTransport *transport,
                                              void                 *payload,
                                              size_t                size,
                                              int                   timeout_ms);
int meta_anland_transport_read_input_fds (MetaAnlandTransport *transport,
                                          int                  *fds,
                                          int                   max_fds,
                                          int                  *n_fds,
                                          int                   timeout_ms);

bool meta_anland_transport_send_output_event (MetaAnlandTransport     *transport,
                                              const struct OutputEvent *event,
                                              const void               *payload,
                                              size_t                    payload_size);

#endif /* META_ANLAND_TRANSPORT_H */
