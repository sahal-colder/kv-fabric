#define _GNU_SOURCE
#include "remote_sock.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#if KVF_HAS_RSOCKETS
#include <rdma/rsocket.h>
#endif

const char *remote_rdma_unavailable(void) {
#if KVF_HAS_RSOCKETS
    int count = 0;
    struct ibv_device **devices = ibv_get_device_list(&count);
    if (!devices)
        return strerror(errno);
    ibv_free_device_list(devices);
    return count ? NULL : "no IB/RoCE devices";
#else
    return "built without rsockets";
#endif
}

static struct sockaddr_in address_for(kvf_endpoint endpoint) {
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(endpoint.port)};
    memcpy(&address.sin_addr, endpoint.ipv4, sizeof(endpoint.ipv4));
    return address;
}

void remote_close(mi_remote_socket s) {
    if (s.fd < 0)
        return;
    if (s.transport == KVF_TRANSPORT_TCP) {
        close(s.fd);
        return;
    }
#if KVF_HAS_RSOCKETS
    rclose(s.fd);
#endif
}

int remote_poll(kvf_transport transport, struct pollfd *fds, nfds_t count, int tcp_timeout_ms) {
    if (transport == KVF_TRANSPORT_TCP)
        return poll(fds, count, tcp_timeout_ms);
#if KVF_HAS_RSOCKETS
    /* rdma-core 39 can hang on a second timed rpoll after an idle timeout. */
    return rpoll(fds, count, 0);
#else
    errno = EOPNOTSUPP;
    return -1;
#endif
}

kvf_status remote_wait(mi_remote_socket s, short events, uint64_t deadline) {
    struct pollfd p = {.fd = s.fd, .events = events};
    for (;;) {
        uint64_t now = kvf_now_ms();
        if (now >= deadline)
            return KVF_TIMEOUT;
        uint64_t remaining = deadline - now;
        int ready = remote_poll(s.transport, &p, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (ready > 0)
            return (p.revents & events) && !(p.revents & POLLNVAL) ? KVF_OK : KVF_IO;
        if (ready < 0 && errno != EINTR)
            return KVF_IO;
        if (!ready && s.transport == KVF_TRANSPORT_RDMA_RSOCKET)
            nanosleep(&(struct timespec){.tv_nsec = 50000}, NULL);
    }
}

#if KVF_HAS_RSOCKETS
static int open_rsocket(void) {
    int fd = rsocket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0 && rfcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        int error = errno;
        rclose(fd);
        errno = error;
        return -1;
    }
    /* Opt-in, set before bind/connect so accepted streams inherit the same
     * bounded resources. IOMAP stays zero: rsend/rrecv do not use riowrite. */
    const char *names[] = {"KVF_RDMA_BUFFER_BYTES", "KVF_RDMA_SQSIZE", "KVF_RDMA_RQSIZE", "KVF_RDMA_INLINE"};
    const int levels[] = {SOL_SOCKET, SOL_RDMA, SOL_RDMA, SOL_RDMA};
    const int options[] = {SO_SNDBUF, RDMA_SQSIZE, RDMA_RQSIZE, RDMA_INLINE};
    for (unsigned i = 0; fd >= 0 && i < 4; ++i) {
        const char *text = getenv(names[i]);
        if (!text) continue;
        char *end;
        errno = 0;
        long value = strtol(text, &end, 10);
        if (errno || *end || value < (i == 3 ? 0 : 1) ||
            value > (i == 0 ? 16*1024*1024 : i == 3 ? 4096 : 16384)) {
            rclose(fd); errno = EINVAL; return -1;
        }
        int size = (int)value;
        if (rsetsockopt(fd, levels[i], options[i], &size, sizeof(size)) < 0 ||
            (i == 0 && rsetsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0)) {
            int error = errno; rclose(fd); errno = error; return -1;
        }
    }
    return fd;
}
#endif

kvf_status remote_listen(kvf_endpoint *endpoint, unsigned lanes, kvf_transport transport,
                          mi_remote_socket *out) {
    struct sockaddr_in address = address_for(*endpoint);
    struct sockaddr *addr = (struct sockaddr *)&address;
    socklen_t len = sizeof(address);
    int enabled = 1;
    mi_remote_socket s = {.fd = -1, .transport = transport};
    if (transport == KVF_TRANSPORT_TCP) {
        s.fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (s.fd < 0 || setsockopt(s.fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
            bind(s.fd, addr, len) < 0 || listen(s.fd, (int)lanes) < 0 || getsockname(s.fd, addr, &len) < 0)
            goto failed;
    } else {
#if KVF_HAS_RSOCKETS
        s.fd = open_rsocket();
        if (s.fd < 0 || rsetsockopt(s.fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
            rbind(s.fd, addr, len) < 0 || rlisten(s.fd, (int)lanes) < 0 || rgetsockname(s.fd, addr, &len) < 0)
            goto failed;
#else
        return KVF_UNAVAILABLE;
#endif
    }
    endpoint->port = ntohs(address.sin_port);
    memcpy(endpoint->ipv4, &address.sin_addr, sizeof(endpoint->ipv4));
    *out = s;
    return KVF_OK;

failed: {
    int error = errno;
    remote_close(s);
    errno = error;
    return KVF_IO;
}
}

mi_remote_socket remote_accept(mi_remote_socket listener) {
    mi_remote_socket client = {.fd = -1, .transport = listener.transport};
    int enabled = 1;
#if KVF_HAS_RSOCKETS
    if (client.transport == KVF_TRANSPORT_RDMA_RSOCKET) {
        client.fd = raccept(listener.fd, NULL, NULL);
        if (client.fd < 0 || (rfcntl(client.fd, F_SETFL, O_NONBLOCK) >= 0 &&
                            rsetsockopt(client.fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0))
            return client;
    } else
#endif
    {
        client.fd = accept4(listener.fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client.fd < 0 || setsockopt(client.fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0)
            return client;
    }
    remote_close(client);
    client.fd = -1;
    return client;
}

kvf_status remote_connect(kvf_endpoint endpoint, kvf_transport transport, uint64_t deadline,
                           mi_remote_socket *out) {
    if (transport == KVF_TRANSPORT_TCP) {
        int fd;
        kvf_status st = kvf_connect(endpoint, deadline, &fd);
        if (st == KVF_OK)
            *out = (mi_remote_socket){.fd = fd, .transport = transport};
        return st;
    }
#if KVF_HAS_RSOCKETS
    if (!endpoint.port)
        return KVF_INVALID;
    if (kvf_now_ms() >= deadline)
        return KVF_TIMEOUT;
    mi_remote_socket s = {.fd = open_rsocket(), .transport = transport};
    if (s.fd < 0)
        return KVF_IO;

    struct sockaddr_in address = address_for(endpoint);
    kvf_status st = KVF_OK;
    if (rconnect(s.fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        st = errno == EINPROGRESS || errno == EALREADY || errno == EINTR ||
             errno == EAGAIN || errno == EWOULDBLOCK ? remote_wait(s, POLLOUT, deadline) : KVF_IO;
        int error;
        socklen_t len = sizeof(error);
        if (st == KVF_OK && (rgetsockopt(s.fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error))
            st = KVF_IO;
    }
    int enabled = 1;
    if (st == KVF_OK && rsetsockopt(s.fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) < 0)
        st = KVF_IO;
    if (st == KVF_OK)
        *out = s;
    else
        remote_close(s);
    return st;
#else
    return KVF_UNAVAILABLE;
#endif
}
