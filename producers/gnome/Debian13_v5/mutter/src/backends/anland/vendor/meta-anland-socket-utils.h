#ifndef META_ANLAND_SOCKET_UTILS_H
#define META_ANLAND_SOCKET_UTILS_H

#include <stddef.h>

int meta_anland_connect_unix (const char *path);

int meta_anland_send_all (int          fd,
                          const void  *buffer,
                          size_t       length);
int meta_anland_recv_all (int    fd,
                          void  *buffer,
                          size_t length);

int meta_anland_send_fds (int          fd,
                          const void  *buffer,
                          size_t       length,
                          const int   *fds,
                          int          n_fds);
int meta_anland_recv_fds (int    fd,
                          void  *buffer,
                          size_t length,
                          int   *fds,
                          int    max_fds,
                          int   *n_fds);

#endif /* META_ANLAND_SOCKET_UTILS_H */
