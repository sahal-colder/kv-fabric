#ifndef MINIINFER_REMOTE_SOCK_H
#define MINIINFER_REMOTE_SOCK_H

#include "kvfabric/wire.h"
#include <poll.h>

/* An rsocket descriptor is only valid with the corresponding r* calls. */
typedef struct {
    int fd;
    kvf_transport transport;
} mi_remote_socket;

unsigned remote_rdma_pending_closes(void);
const char *remote_rdma_unavailable(void);
kvf_status remote_listen(kvf_endpoint *endpoint, unsigned lanes, kvf_transport transport,
                          mi_remote_socket *out);
mi_remote_socket remote_accept(mi_remote_socket listener);
kvf_status remote_connect(kvf_endpoint endpoint, kvf_transport transport, uint64_t deadline,
                           mi_remote_socket *out);
int remote_poll(kvf_transport transport, struct pollfd *fds, nfds_t count, int tcp_timeout_ms);
kvf_status remote_wait(mi_remote_socket socket, short events, uint64_t deadline);
void remote_close(mi_remote_socket socket);

#endif
