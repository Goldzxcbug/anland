#include "meta-anland-socket-utils.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int
meta_anland_connect_unix (const char *path)
{
  struct sockaddr_un address = { 0 };
  int fd;

  if (!path || strlen (path) >= sizeof (address.sun_path))
    {
      errno = ENAMETOOLONG;
      return -1;
    }

  fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;

  address.sun_family = AF_UNIX;
  strcpy (address.sun_path, path);
  if (connect (fd, (struct sockaddr *) &address, sizeof (address)) < 0)
    {
      close (fd);
      return -1;
    }

  return fd;
}

int
meta_anland_send_all (int         fd,
                      const void *buffer,
                      size_t      length)
{
  const uint8_t *data = buffer;
  size_t sent = 0;

  while (sent < length)
    {
      ssize_t n = send (fd, data + sent, length - sent, MSG_NOSIGNAL);

      if (n > 0)
        {
          sent += n;
          continue;
        }
      if (n < 0 && errno == EINTR)
        continue;

      return -1;
    }

  return 0;
}

int
meta_anland_recv_all (int    fd,
                      void  *buffer,
                      size_t length)
{
  uint8_t *data = buffer;
  size_t received = 0;

  while (received < length)
    {
      ssize_t n = recv (fd, data + received, length - received, 0);

      if (n > 0)
        {
          received += n;
          continue;
        }
      if (n < 0 && errno == EINTR)
        continue;

      return -1;
    }

  return 0;
}

int
meta_anland_send_fds (int         fd,
                      const void *buffer,
                      size_t      length,
                      const int  *fds,
                      int         n_fds)
{
  struct iovec iov = { .iov_base = (void *) buffer, .iov_len = length };
  struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1 };
  struct cmsghdr *cmsg;
  size_t control_length;
  char *control;
  ssize_t n;

  if (!buffer || length == 0 || !fds || n_fds <= 0)
    {
      errno = EINVAL;
      return -1;
    }

  control_length = CMSG_SPACE (sizeof (int) * (size_t) n_fds);
  control = calloc (1, control_length);
  if (!control)
    return -1;

  message.msg_control = control;
  message.msg_controllen = control_length;
  cmsg = CMSG_FIRSTHDR (&message);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN (sizeof (int) * (size_t) n_fds);
  memcpy (CMSG_DATA (cmsg), fds, sizeof (int) * (size_t) n_fds);

  do
    n = sendmsg (fd, &message, MSG_NOSIGNAL);
  while (n < 0 && errno == EINTR);

  free (control);
  return n == (ssize_t) length ? 0 : -1;
}

int
meta_anland_recv_fds (int    fd,
                      void  *buffer,
                      size_t length,
                      int   *fds,
                      int    max_fds,
                      int   *n_fds)
{
  struct iovec iov = { .iov_base = buffer, .iov_len = length };
  struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1 };
  size_t control_length;
  char *control;
  ssize_t n;

  if (!buffer || length == 0 || !fds || max_fds <= 0 || !n_fds)
    {
      errno = EINVAL;
      return -1;
    }

  *n_fds = 0;
  control_length = CMSG_SPACE (sizeof (int) * (size_t) max_fds);
  control = calloc (1, control_length);
  if (!control)
    return -1;

  message.msg_control = control;
  message.msg_controllen = control_length;
  do
    n = recvmsg (fd, &message, MSG_CMSG_CLOEXEC);
  while (n < 0 && errno == EINTR);

  if (n <= 0)
    goto fail;

  for (struct cmsghdr *cmsg = CMSG_FIRSTHDR (&message);
       cmsg;
       cmsg = CMSG_NXTHDR (&message, cmsg))
    {
      int count;
      int available;
      int copy_count;

      if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
        continue;
      if (cmsg->cmsg_len < CMSG_LEN (0) ||
          (cmsg->cmsg_len - CMSG_LEN (0)) % sizeof (int) != 0)
        goto fail;

      count = (int) ((cmsg->cmsg_len - CMSG_LEN (0)) / sizeof (int));
      available = max_fds - *n_fds;
      copy_count = count < available ? count : available;
      memcpy (fds + *n_fds, CMSG_DATA (cmsg),
              sizeof (int) * (size_t) copy_count);
      *n_fds += copy_count;
      for (int i = copy_count; i < count; i++)
        {
          int received_fd;

          memcpy (&received_fd, (int *) CMSG_DATA (cmsg) + i,
                  sizeof (received_fd));
          close (received_fd);
        }
    }

  if (!(message.msg_flags & MSG_CTRUNC))
    {
      free (control);
      return (int) n;
    }

  errno = EMSGSIZE;

fail:
  for (int i = 0; i < *n_fds; i++)
    close (fds[i]);
  *n_fds = 0;
  free (control);
  return -1;
}
