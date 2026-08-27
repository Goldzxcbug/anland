#include "meta-anland-transport.h"

#include "meta-anland-socket-utils.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define HANDSHAKE_TIMEOUT_MS 100
#define INITIAL_HANDSHAKE_TIMEOUT_MS 5000
#define INPUT_TIMEOUT_MS 100

struct _MetaAnlandTransport
{
  int control_fd;
  int data_fd;
  int buffer_ready_fd;
  int fence_fd;
  int shm_fd;
  int audio_fd;
  int pending_render_fence_fd;
  volatile uint32_t *selected_buffer;

  struct screen_info screen_info;
  int buffer_fds[MAX_BUFS];
  struct buf_info buffer_infos[MAX_BUFS];
  int n_buffers;
  bool fallback;

  void (*fallback_callback) (void *user_data);
  void *fallback_user_data;
};

static void
close_fds (int *fds,
           int  n_fds)
{
  for (int i = 0; i < n_fds; i++)
    {
      if (fds[i] >= 0)
        close (fds[i]);
    }
}

static void
release_consumer_resources (MetaAnlandTransport *transport)
{
  for (int i = 0; i < transport->n_buffers; i++)
    {
      if (transport->buffer_fds[i] >= 0)
        {
          close (transport->buffer_fds[i]);
          transport->buffer_fds[i] = -1;
        }
    }
  transport->n_buffers = 0;

  if (transport->selected_buffer)
    {
      munmap ((void *) transport->selected_buffer, sizeof (uint32_t));
      transport->selected_buffer = NULL;
    }

  if (transport->data_fd >= 0)
    close (transport->data_fd);
  if (transport->buffer_ready_fd >= 0)
    close (transport->buffer_ready_fd);
  if (transport->fence_fd >= 0)
    close (transport->fence_fd);
  if (transport->shm_fd >= 0)
    close (transport->shm_fd);
  if (transport->audio_fd >= 0)
    close (transport->audio_fd);
  if (transport->pending_render_fence_fd >= 0)
    close (transport->pending_render_fence_fd);

  transport->data_fd = -1;
  transport->buffer_ready_fd = -1;
  transport->fence_fd = -1;
  transport->shm_fd = -1;
  transport->audio_fd = -1;
  transport->pending_render_fence_fd = -1;
}

static void
enter_fallback (MetaAnlandTransport *transport)
{
  if (transport->fallback)
    return;

  transport->fallback = true;
  if (transport->fallback_callback)
    transport->fallback_callback (transport->fallback_user_data);
  release_consumer_resources (transport);
}

static int
wait_for_fd (int fd,
             short events,
             int timeout_ms)
{
  struct pollfd poll_fd = { .fd = fd, .events = events };
  int result;

  do
    result = poll (&poll_fd, 1, timeout_ms);
  while (result < 0 && errno == EINTR);

  if (result <= 0)
    return result;
  if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL))
    return -1;
  return poll_fd.revents & events ? 1 : -1;
}

static int
recv_all_timeout (int    fd,
                  void  *buffer,
                  size_t size,
                  int    timeout_ms)
{
  uint8_t *data = buffer;
  size_t received = 0;

  while (received < size)
    {
      ssize_t n = recv (fd, data + received, size - received, MSG_DONTWAIT);

      if (n > 0)
        {
          received += n;
          continue;
        }
      if (n == 0)
        return -1;
      if (errno == EINTR)
        continue;
      if (errno != EAGAIN)
        return -1;
      if (wait_for_fd (fd, POLLIN, timeout_ms) != 1)
        return -1;
    }

  return 0;
}

static int
send_all_timeout (int         fd,
                  const void *buffer,
                  size_t      size,
                  int         timeout_ms)
{
  const uint8_t *data = buffer;
  size_t sent = 0;

  while (sent < size)
    {
      ssize_t n = send (fd, data + sent, size - sent,
                        MSG_NOSIGNAL | MSG_DONTWAIT);

      if (n > 0)
        {
          sent += n;
          continue;
        }
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0 && errno == EAGAIN &&
          wait_for_fd (fd, POLLOUT, timeout_ms) == 1)
        continue;

      return -1;
    }

  return 0;
}

static int
recv_fds_exact (int    fd,
                void  *buffer,
                size_t size,
                int   *fds,
                int    max_fds,
                int   *n_fds)
{
  int received;

  received = meta_anland_recv_fds (fd, buffer, size, fds, max_fds, n_fds);
  if (received < 0)
    return -1;
  if ((size_t) received < size &&
      meta_anland_recv_all (fd, (uint8_t *) buffer + received,
                            size - (size_t) received) < 0)
    return -1;

  return 0;
}

static int
discard_data_payload (MetaAnlandTransport *transport,
                      uint32_t             size)
{
  uint8_t buffer[4096];

  if (size > META_ANLAND_MAX_PAYLOAD_SIZE)
    return -1;

  while (size > 0)
    {
      size_t chunk = size < sizeof (buffer) ? size : sizeof (buffer);

      if (recv_all_timeout (transport->data_fd, buffer, chunk,
                            INPUT_TIMEOUT_MS) < 0)
        return -1;
      size -= (uint32_t) chunk;
    }

  return 0;
}

static MetaAnlandTransportPickupResult
pickup_fds (MetaAnlandTransport *transport)
{
  struct ctrl_msg request = { .type = CTRL_MSG_PICKUP_FDS, .size = 0 };
  struct ctrl_msg response;
  int fds[5] = { -1, -1, -1, -1, -1 };
  int n_fds = 0;
  int wait_result;

  if (meta_anland_send_all (transport->control_fd, &request,
                            sizeof (request)) < 0)
    return META_ANLAND_TRANSPORT_PICKUP_DAEMON_LOST;

  wait_result = wait_for_fd (transport->control_fd, POLLIN,
                             HANDSHAKE_TIMEOUT_MS);
  if (wait_result == 0)
    return META_ANLAND_TRANSPORT_PICKUP_NO_CONSUMER;
  if (wait_result < 0 ||
      recv_fds_exact (transport->control_fd, &response, sizeof (response),
                      fds, 5, &n_fds) < 0)
    {
      close_fds (fds, n_fds);
      return META_ANLAND_TRANSPORT_PICKUP_DAEMON_LOST;
    }

  if (response.type != CTRL_MSG_FDS_READY || response.size != 0 || n_fds != 5)
    {
      close_fds (fds, n_fds);
      return META_ANLAND_TRANSPORT_PICKUP_PROTOCOL_ERROR;
    }

  transport->buffer_ready_fd = fds[0];
  transport->fence_fd = fds[1];
  transport->data_fd = fds[2];
  transport->shm_fd = fds[3];
  transport->audio_fd = fds[4];
  transport->selected_buffer = mmap (NULL, sizeof (uint32_t), PROT_READ,
                                     MAP_SHARED, transport->shm_fd, 0);
  if (transport->selected_buffer == MAP_FAILED)
    {
      transport->selected_buffer = NULL;
      return META_ANLAND_TRANSPORT_PICKUP_PROTOCOL_ERROR;
    }

  return META_ANLAND_TRANSPORT_PICKUP_READY;
}

static MetaAnlandTransportPickupResult
receive_buffers (MetaAnlandTransport *transport)
{
  struct data_msg header;
  struct buf_info buffer_infos[MAX_BUFS];
  int fds[MAX_BUFS] = { -1, -1, -1, -1, -1, -1, -1, -1 };
  int n_fds = 0;
  int n_buffers;

  if (wait_for_fd (transport->data_fd, POLLIN, HANDSHAKE_TIMEOUT_MS) != 1 ||
      recv_fds_exact (transport->data_fd, &header, sizeof (header),
                      fds, MAX_BUFS, &n_fds) < 0)
    {
      close_fds (fds, n_fds);
      return META_ANLAND_TRANSPORT_PICKUP_NO_CONSUMER;
    }

  if (header.type != DATA_MSG_BUFS_READY || header.size == 0 ||
      header.size % sizeof (struct buf_info) != 0)
    goto protocol_error;

  n_buffers = (int) (header.size / sizeof (struct buf_info));
  if (n_buffers != n_fds || n_buffers < 1 || n_buffers > MAX_BUFS ||
      meta_anland_recv_all (transport->data_fd, buffer_infos,
                            header.size) < 0)
    goto protocol_error;

  for (int i = 0; i < n_buffers; i++)
    {
      if (buffer_infos[i].width == 0 || buffer_infos[i].height == 0 ||
          buffer_infos[i].stride == 0 ||
          (i > 0 && (buffer_infos[i].width != buffer_infos[0].width ||
                     buffer_infos[i].height != buffer_infos[0].height ||
                     buffer_infos[i].format != buffer_infos[0].format)))
        goto protocol_error;
    }

  for (int i = 0; i < n_buffers; i++)
    {
      transport->buffer_fds[i] = fds[i];
      transport->buffer_infos[i] = buffer_infos[i];
    }
  transport->n_buffers = n_buffers;
  return META_ANLAND_TRANSPORT_PICKUP_READY;

protocol_error:
  close_fds (fds, n_fds);
  return META_ANLAND_TRANSPORT_PICKUP_PROTOCOL_ERROR;
}

int
meta_anland_transport_connect (MetaAnlandTransport **out_transport,
                               const char           *socket_path)
{
  MetaAnlandTransport *transport;
  struct ctrl_msg request = { .type = CTRL_MSG_PRODUCER_HELLO, .size = 0 };
  struct ctrl_msg response;
  struct screen_info screen_info;
  int wait_result;

  if (!out_transport || !socket_path)
    return -1;

  *out_transport = NULL;
  transport = calloc (1, sizeof (*transport));
  if (!transport)
    return -1;

  transport->control_fd = -1;
  transport->data_fd = -1;
  transport->buffer_ready_fd = -1;
  transport->fence_fd = -1;
  transport->shm_fd = -1;
  transport->audio_fd = -1;
  transport->pending_render_fence_fd = -1;
  transport->fallback = true;
  for (int i = 0; i < MAX_BUFS; i++)
    transport->buffer_fds[i] = -1;

  transport->control_fd = meta_anland_connect_unix (socket_path);
  if (transport->control_fd < 0 ||
      meta_anland_send_all (transport->control_fd, &request,
                            sizeof (request)) < 0)
    goto failed;

  wait_result = wait_for_fd (transport->control_fd, POLLIN,
                             INITIAL_HANDSHAKE_TIMEOUT_MS);
  if (wait_result != 1 ||
      recv_all_timeout (transport->control_fd, &response, sizeof (response),
                        INITIAL_HANDSHAKE_TIMEOUT_MS) < 0 ||
      response.type != CTRL_MSG_SCREEN_INFO ||
      response.size != sizeof (screen_info) ||
      recv_all_timeout (transport->control_fd, &screen_info,
                        sizeof (screen_info), INITIAL_HANDSHAKE_TIMEOUT_MS) < 0)
    goto failed;

  transport->screen_info = screen_info;
  *out_transport = transport;
  return 0;

failed:
  if (transport->control_fd >= 0)
    close (transport->control_fd);
  free (transport);
  return -1;
}

void
meta_anland_transport_disconnect (MetaAnlandTransport *transport)
{
  if (!transport)
    return;

  release_consumer_resources (transport);
  if (transport->control_fd >= 0)
    close (transport->control_fd);
  free (transport);
}

bool
meta_anland_transport_get_screen_info (MetaAnlandTransport *transport,
                                       struct screen_info   *screen_info)
{
  if (!transport || !screen_info)
    return false;

  *screen_info = transport->screen_info;
  return true;
}

MetaAnlandTransportPickupResult
meta_anland_transport_try_pickup (MetaAnlandTransport *transport)
{
  MetaAnlandTransportPickupResult result;

  if (!transport)
    return META_ANLAND_TRANSPORT_PICKUP_PROTOCOL_ERROR;
  if (!transport->fallback)
    return META_ANLAND_TRANSPORT_PICKUP_READY;

  result = pickup_fds (transport);
  if (result != META_ANLAND_TRANSPORT_PICKUP_READY)
    {
      release_consumer_resources (transport);
      return result;
    }

  result = receive_buffers (transport);
  if (result != META_ANLAND_TRANSPORT_PICKUP_READY)
    {
      release_consumer_resources (transport);
      return result;
    }

  transport->fallback = false;
  return META_ANLAND_TRANSPORT_PICKUP_READY;
}

bool
meta_anland_transport_is_fallback (MetaAnlandTransport *transport)
{
  return !transport || transport->fallback;
}

void
meta_anland_transport_enter_fallback (MetaAnlandTransport *transport)
{
  if (transport)
    enter_fallback (transport);
}

void
meta_anland_transport_set_fallback_callback (MetaAnlandTransport *transport,
                                             void                 (*callback) (void *user_data),
                                             void                  *user_data)
{
  transport->fallback_callback = callback;
  transport->fallback_user_data = user_data;
}

int
meta_anland_transport_get_buffer_ready_fd (MetaAnlandTransport *transport)
{
  return transport && !transport->fallback ? transport->buffer_ready_fd : -1;
}

int
meta_anland_transport_get_data_fd (MetaAnlandTransport *transport)
{
  return transport && !transport->fallback ? transport->data_fd : -1;
}

int
meta_anland_transport_get_audio_fd (MetaAnlandTransport *transport)
{
  return transport && !transport->fallback ? transport->audio_fd : -1;
}

int
meta_anland_transport_get_buffer_count (MetaAnlandTransport *transport)
{
  return transport && !transport->fallback ? transport->n_buffers : 0;
}

int
meta_anland_transport_get_selected_buffer (MetaAnlandTransport *transport)
{
  uint32_t selected;

  if (!transport || transport->fallback || !transport->selected_buffer)
    return -1;

  selected = *transport->selected_buffer;
  return selected < (uint32_t) transport->n_buffers ? (int) selected : -1;
}

int
meta_anland_transport_get_buffer_fd (MetaAnlandTransport *transport,
                                     int                   index)
{
  if (!transport || transport->fallback || index < 0 ||
      index >= transport->n_buffers)
    return -1;

  return transport->buffer_fds[index];
}

bool
meta_anland_transport_get_buffer_info (MetaAnlandTransport *transport,
                                       int                   index,
                                       struct buf_info      *buffer_info)
{
  if (!transport || !buffer_info || transport->fallback || index < 0 ||
      index >= transport->n_buffers)
    return false;

  *buffer_info = transport->buffer_infos[index];
  return true;
}

void
meta_anland_transport_set_render_fence (MetaAnlandTransport *transport,
                                        int                   fence_fd)
{
  if (!transport)
    {
      if (fence_fd >= 0)
        close (fence_fd);
      return;
    }

  if (transport->pending_render_fence_fd >= 0)
    close (transport->pending_render_fence_fd);
  transport->pending_render_fence_fd = fence_fd;
}

bool
meta_anland_transport_notify_frame_done (MetaAnlandTransport *transport)
{
  struct iovec iov;
  struct msghdr message = { 0 };
  char byte = 0;
  union
  {
    char bytes[CMSG_SPACE (sizeof (int))];
    struct cmsghdr align;
  } control = { 0 };
  ssize_t n;

  if (!transport)
    return false;
  if (transport->fallback)
    {
      if (transport->pending_render_fence_fd >= 0)
        close (transport->pending_render_fence_fd);
      transport->pending_render_fence_fd = -1;
      return false;
    }

  iov.iov_base = &byte;
  iov.iov_len = sizeof (byte);
  message.msg_iov = &iov;
  message.msg_iovlen = 1;
  if (transport->pending_render_fence_fd >= 0)
    {
      struct cmsghdr *cmsg;

      message.msg_control = control.bytes;
      message.msg_controllen = sizeof (control.bytes);
      cmsg = CMSG_FIRSTHDR (&message);
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_RIGHTS;
      cmsg->cmsg_len = CMSG_LEN (sizeof (int));
      memcpy (CMSG_DATA (cmsg), &transport->pending_render_fence_fd,
              sizeof (transport->pending_render_fence_fd));
    }

  do
    n = sendmsg (transport->fence_fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
  while (n < 0 && errno == EINTR);

  if (transport->pending_render_fence_fd >= 0)
    {
      close (transport->pending_render_fence_fd);
      transport->pending_render_fence_fd = -1;
    }

  if (n == 1)
    return true;

  enter_fallback (transport);
  return false;
}

int
meta_anland_transport_poll_input_event (MetaAnlandTransport *transport,
                                        struct InputEvent    *event,
                                        int                   timeout_ms)
{
  struct data_msg header;

  if (!transport || !event || transport->fallback)
    return 0;

  for (;;)
    {
      int wait_result = wait_for_fd (transport->data_fd, POLLIN, timeout_ms);

      if (wait_result == 0)
        return 0;
      if (wait_result < 0 ||
          recv_all_timeout (transport->data_fd, &header, sizeof (header),
                            INPUT_TIMEOUT_MS) < 0)
        {
          enter_fallback (transport);
          return -1;
        }

      if (header.type != DATA_MSG_INPUT_EVENT)
        {
          if (discard_data_payload (transport, header.size) < 0)
            {
              enter_fallback (transport);
              return -1;
            }
          timeout_ms = 0;
          continue;
        }

      if (header.size != sizeof (*event) ||
          recv_all_timeout (transport->data_fd, event, sizeof (*event),
                            INPUT_TIMEOUT_MS) < 0)
        {
          enter_fallback (transport);
          return -1;
        }

      return 1;
    }
}

int
meta_anland_transport_read_input_payload (MetaAnlandTransport *transport,
                                          void                 *payload,
                                          size_t                size,
                                          int                   timeout_ms)
{
  if (!transport || transport->fallback)
    return 0;
  if (size == 0)
    return 1;
  if (size > META_ANLAND_MAX_PAYLOAD_SIZE || !payload ||
      recv_all_timeout (transport->data_fd, payload, size,
                                    timeout_ms) < 0)
    {
      enter_fallback (transport);
      return -1;
    }

  return 1;
}

int
meta_anland_transport_read_input_fds (MetaAnlandTransport *transport,
                                      int                  *fds,
                                      int                   max_fds,
                                      int                  *n_fds,
                                      int                   timeout_ms)
{
  struct data_msg header;

  if (!n_fds)
    return -1;
  *n_fds = 0;
  if (!transport || transport->fallback)
    return 0;
  if (wait_for_fd (transport->data_fd, POLLIN, timeout_ms) != 1 ||
      recv_fds_exact (transport->data_fd, &header, sizeof (header), fds,
                      max_fds, n_fds) < 0 ||
      header.type != DATA_MSG_INPUT_EXTEND_FDS || header.size != 0 ||
      *n_fds < 1)
    {
      close_fds (fds, *n_fds);
      *n_fds = 0;
      enter_fallback (transport);
      return -1;
    }

  return 1;
}

bool
meta_anland_transport_send_output_event (MetaAnlandTransport     *transport,
                                         const struct OutputEvent *event,
                                         const void               *payload,
                                         size_t                    payload_size)
{
  struct data_msg header = { .type = DATA_MSG_OUTPUT_EVENT,
                             .size = sizeof (*event) };
  size_t size;
  uint8_t *message;

  if (!transport || !event || transport->fallback ||
      (payload_size > 0 && !payload) ||
      payload_size > META_ANLAND_MAX_PAYLOAD_SIZE ||
      payload_size > SIZE_MAX - sizeof (header) - sizeof (*event))
    return false;

  size = sizeof (header) + sizeof (*event) + payload_size;
  message = malloc (size);
  if (!message)
    return false;

  memcpy (message, &header, sizeof (header));
  memcpy (message + sizeof (header), event, sizeof (*event));
  if (payload_size > 0)
    memcpy (message + sizeof (header) + sizeof (*event), payload, payload_size);

  if (send_all_timeout (transport->data_fd, message, size,
                        INPUT_TIMEOUT_MS) < 0)
    {
      free (message);
      enter_fallback (transport);
      return false;
    }

  free (message);
  return true;
}
