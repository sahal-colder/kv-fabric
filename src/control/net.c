/*
 * TCP framing and bounded listeners. Servers own listening and accepted sockets.
 * Frames borrow bytes only for the call. No Directory/Storage locks enter this layer.
 */
#define _GNU_SOURCE
#include "kvfabric/wire.h"
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define KVF_MAGIC UINT32_C(0x4b564631)

struct kvf_server {
    struct sockaddr_in address;
    int fd;
    unsigned started;
    uint32_t timeout_ms;
    atomic_int stopping;
    pthread_mutex_t accept_lock;  /* Only one listener lane polls the accept queue at a time. */
    kvf_connection_fn handle;
    void *context;
    struct lane {
        pthread_t thread;
        struct kvf_server *server;
        unsigned index;
    } lane[];
};

uint64_t kvf_now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
        return 0;
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

uint64_t kvf_now_ms(void) {
    return kvf_now_ns() / 1000000;
}

static struct sockaddr_in address_for(kvf_endpoint ep) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(ep.port)};
    memcpy(&a.sin_addr, ep.ipv4, sizeof(ep.ipv4));
    return a;
}

static kvf_status wait_for(int fd, short events, uint64_t deadline) {
    struct pollfd p = {.fd = fd, .events = events};
    for (;;) {
        uint64_t now = kvf_now_ms();
        if (deadline <= now)
            return KVF_TIMEOUT;
        uint64_t remaining = deadline - now;
        int n = poll(&p, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (n > 0)
            return p.revents & POLLNVAL ? KVF_IO : KVF_OK;
        if (n < 0 && errno != EINTR)
            return KVF_IO;
    }
}

kvf_status kvf_connect(kvf_endpoint ep, uint64_t deadline, int *out) {
    if (!ep.port || !out)
        return KVF_INVALID;
    if (deadline <= kvf_now_ms())
        return KVF_TIMEOUT;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return KVF_IO;
    struct sockaddr_in address = address_for(ep);
    kvf_status st = KVF_OK;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        st = errno == EINPROGRESS || errno == EINTR ? wait_for(fd, POLLOUT, deadline) : KVF_IO;
        int error;
        socklen_t len = sizeof(error);
        if (st == KVF_OK && (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error))
            st = KVF_IO;
    }
    int enabled = 1;
    if (st == KVF_OK && setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0)
        st = KVF_IO;
    if (st == KVF_OK)
        *out = fd;
    else
        close(fd);
    return st;
}

static kvf_status receive(int fd, void *dst, size_t size, uint64_t deadline, uint64_t *received) {
    size_t offset = 0;
    while (offset < size) {
        if (deadline <= kvf_now_ms())
            return KVF_TIMEOUT;
        ssize_t n = recv(fd, (uint8_t *)dst + offset, size - offset, MSG_DONTWAIT);
        if (n > 0) {
            offset += (size_t)n;
            if (received)
                *received += (uint64_t)n;
            continue;
        }
        if (!n)
            return KVF_IO;
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return KVF_IO;
        kvf_status st = wait_for(fd, POLLIN, deadline);
        if (st != KVF_OK)
            return st;
    }
    return KVF_OK;
}

static int valid_frame(kvf_frame f) {
    int tensor = f.opcode == KVF_FETCH || f.opcode == KVF_FETCH_STREAM;
    uint32_t max = KVF_METADATA_BYTES;
    if (f.response && tensor)
        max = KVF_MAX_LAYER_BYTES;
    if (f.response && f.opcode == KVF_FETCH_BATCH)
        max = KVF_MAX_BATCH_BYTES;
    if (f.response && f.opcode == KVF_ACQUIRE_BATCH)
        max = KVF_BATCH_GRANT_BYTES;
    return ((f.opcode >= KVF_REGISTER && f.opcode <= KVF_FINISH_BATCH) ||
            (f.opcode >= KVF_FETCH && f.opcode <= KVF_FETCH_BATCH)) &&
        (f.response == 0 || f.response == 1) && (unsigned)f.status <= KVF_CANCELLED &&
        f.request_id && f.size <= max && (f.status == KVF_OK || (f.response && !f.size));
}

kvf_status kvf_send_frame(int fd, kvf_frame f, const void *payload, uint64_t deadline) {
    if (!valid_frame(f) || (!payload && f.size))
        return KVF_INVALID;
    uint8_t header[KVF_HEADER_BYTES];
    kvf_writer w = {.buffer = header, .capacity = sizeof(header)};
    kvf_put_u32(&w, KVF_MAGIC);
    kvf_put_u16(&w, KVF_WIRE_VERSION);
    kvf_put_u16(&w, KVF_HEADER_BYTES);
    kvf_put_u16(&w, (uint16_t)f.opcode);
    kvf_put_u16(&w, (uint16_t)f.response);
    kvf_put_u32(&w, (uint32_t)f.status);
    kvf_put_u64(&w, f.request_id);
    kvf_put_u32(&w, f.size);
    kvf_put_u32(&w, kvf_crc32c(payload, f.size));
    kvf_put_u32(&w, kvf_crc32c(header, w.size));
    struct iovec parts[] = {{header, sizeof(header)}, {(void *)payload, f.size}};
    struct msghdr msg = {.msg_iov = parts, .msg_iovlen = f.size ? 2 : 1};
    while (msg.msg_iovlen) {
        if (deadline <= kvf_now_ms())
            return KVF_TIMEOUT;
        ssize_t n = sendmsg(fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            size_t consumed = (size_t)n;
            while (msg.msg_iovlen && consumed >= msg.msg_iov->iov_len) {
                consumed -= msg.msg_iov->iov_len;
                ++msg.msg_iov;
                --msg.msg_iovlen;
            }
            if (consumed) {
                msg.msg_iov->iov_base = (uint8_t *)msg.msg_iov->iov_base + consumed;
                msg.msg_iov->iov_len -= consumed;
            }
        } else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            return KVF_IO;
        } else if (errno != EINTR) {
            kvf_status st = wait_for(fd, POLLOUT, deadline);
            if (st != KVF_OK)
                return st;
        }
    }
    return KVF_OK;
}

static kvf_status recv_frame(int fd, kvf_frame *frame, void *payload, size_t capacity,
                             uint64_t deadline, kvf_receive_timing *timing) {
    if (!frame)
        return KVF_INVALID;
    uint8_t header[KVF_HEADER_BYTES];
    uint64_t start = timing ? kvf_now_ns() : 0;
    kvf_status st = receive(fd, header, sizeof(header), deadline, NULL);
    if (timing) {
        uint64_t elapsed = kvf_now_ns() - start;
        timing->net_recv_ns += elapsed;
        timing->reply_wait_ns += elapsed;
    }
    if (st != KVF_OK)
        return st;
    kvf_reader r = {.buffer = header, .size = sizeof(header)};
    if (kvf_get_u32(&r) != KVF_MAGIC || kvf_get_u16(&r) != KVF_WIRE_VERSION ||
        kvf_get_u16(&r) != KVF_HEADER_BYTES)
        return KVF_PROTOCOL;
    kvf_frame f;
    f.opcode = (kvf_opcode)kvf_get_u16(&r);
    f.response = kvf_get_u16(&r);
    uint32_t status = kvf_get_u32(&r);
    if (status > KVF_CANCELLED)
        return KVF_PROTOCOL;
    f.status = (kvf_status)status;
    f.request_id = kvf_get_u64(&r);
    f.size = kvf_get_u32(&r);
    uint32_t payload_crc = kvf_get_u32(&r);
    uint32_t header_crc = kvf_get_u32(&r);
    start = timing ? kvf_now_ns() : 0;
    uint32_t actual = kvf_crc32c(header, sizeof(header) - sizeof(header_crc));
    if (timing)
        timing->crc_verify_ns += kvf_now_ns() - start;
    if (actual != header_crc)
        return KVF_CRC;
    if (!valid_frame(f) || f.size > capacity || (f.size && !payload))
        return KVF_PROTOCOL;
    start = timing ? kvf_now_ns() : 0;
    st = receive(fd, payload, f.size, deadline, timing ? &timing->received_bytes : NULL);
    if (timing)
        timing->net_recv_ns += kvf_now_ns() - start;
    if (st != KVF_OK)
        return st;
    start = timing ? kvf_now_ns() : 0;
    actual = kvf_crc32c(payload, f.size);
    if (timing)
        timing->crc_verify_ns += kvf_now_ns() - start;
    if (actual != payload_crc)
        return KVF_CRC;
    *frame = f;
    return KVF_OK;
}

kvf_status kvf_recv_frame(int fd, kvf_frame *frame, void *payload, size_t capacity, uint64_t deadline) {
    return recv_frame(fd, frame, payload, capacity, deadline, NULL);
}

kvf_status kvf_rpc_on_timed(int fd, kvf_opcode op, const kvf_writer *request,
                            void *reply, size_t capacity, size_t *reply_size, uint64_t deadline,
                            kvf_receive_timing *timing) {
    static _Atomic uint64_t next = 1;
    if (request && (request->failed || request->size > KVF_METADATA_BYTES))
        return KVF_INVALID;
    uint64_t id = atomic_fetch_add_explicit(&next, 1, memory_order_relaxed);
    kvf_frame f = {.opcode = op, .request_id = id, .size = request ? (uint32_t)request->size : 0};
    kvf_status st = kvf_send_frame(fd, f, request ? request->buffer : NULL, deadline);
    if (st == KVF_OK)
        st = recv_frame(fd, &f, reply, capacity, deadline, timing);
    if (st != KVF_OK)
        return st;
    if (!f.response || f.opcode != op || f.request_id != id)
        return KVF_PROTOCOL;
    if (reply_size)
        *reply_size = f.size;
    return f.status;
}

kvf_status kvf_rpc_on(int fd, kvf_opcode op, const kvf_writer *request,
                     void *reply, size_t capacity, size_t *reply_size, uint64_t deadline) {
    return kvf_rpc_on_timed(fd, op, request, reply, capacity, reply_size, deadline, NULL);
}

kvf_status kvf_rpc(kvf_endpoint ep, kvf_opcode op, const kvf_writer *request,
                    void *reply, size_t capacity, size_t *reply_size, uint32_t timeout_ms) {
    if (!timeout_ms || (request && (request->failed || request->size > KVF_METADATA_BYTES)))
        return KVF_INVALID;
    uint64_t deadline = kvf_now_ms() + timeout_ms;
    int fd;
    kvf_status st = kvf_connect(ep, deadline, &fd);
    if (st != KVF_OK)
        return st;
    st = kvf_rpc_on(fd, op, request, reply, capacity, reply_size, deadline);
    close(fd);
    return st;
}

static void *serve(void *arg) {
    struct lane *lane = arg;
    kvf_server *s = lane->server;
    for (;;) {
        pthread_mutex_lock(&s->accept_lock);
        if (atomic_load_explicit(&s->stopping, memory_order_acquire)) {
            pthread_mutex_unlock(&s->accept_lock);
            break;
        }
        if (wait_for(s->fd, POLLIN, kvf_now_ms() + 100) != KVF_OK) {
            pthread_mutex_unlock(&s->accept_lock);
            continue;
        }
        int fd = accept4(s->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        int accept_error = fd < 0 ? errno : 0;
        pthread_mutex_unlock(&s->accept_lock);
        if (fd < 0) {
            if (accept_error == EINTR || accept_error == EAGAIN || accept_error == EWOULDBLOCK ||
                accept_error == ECONNABORTED)
                continue;
            struct timespec delay = {.tv_nsec = 10000000};
            nanosleep(&delay, NULL);
            continue;
        }
        int enabled = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0)
            s->handle(s->context, lane->index, fd, kvf_now_ms() + s->timeout_ms);
        close(fd);
    }
    return NULL;
}

kvf_status kvf_server_start(kvf_endpoint listen_on, unsigned lanes, uint32_t timeout_ms,
                             kvf_connection_fn handle, void *context, kvf_server **out) {
    if (!lanes || lanes > 64 || !timeout_ms || timeout_ms > 30000 || !handle || !out)
        return KVF_INVALID;
    kvf_server *s = calloc(1, sizeof(*s) + lanes * sizeof(*s->lane));
    if (!s)
        return KVF_LIMIT;
    if (pthread_mutex_init(&s->accept_lock, NULL) != 0) {
        free(s);
        return KVF_INTERNAL;
    }
    s->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    s->address = address_for(listen_on);
    s->handle = handle;
    s->context = context;
    s->timeout_ms = timeout_ms;
    atomic_init(&s->stopping, 0);
    int enabled = 1;
    socklen_t len = sizeof(s->address);
    kvf_status st = KVF_IO;
    if (s->fd < 0 || setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0 ||
        bind(s->fd, (struct sockaddr *)&s->address, len) < 0 ||
        listen(s->fd, (int)lanes) < 0 || getsockname(s->fd, (struct sockaddr *)&s->address, &len) < 0)
        goto failed;

    st = KVF_INTERNAL;
    for (unsigned i = 0; i < lanes; ++i) {
        s->lane[i].server = s;
        s->lane[i].index = i;
        if (pthread_create(&s->lane[i].thread, NULL, serve, &s->lane[i]) != 0)
            goto failed;
        ++s->started;
    }
    *out = s;
    return KVF_OK;

failed:
    kvf_server_stop(s);
    return st;
}

kvf_endpoint kvf_server_endpoint(const kvf_server *s) {
    kvf_endpoint e = {.port = ntohs(s->address.sin_port)};
    memcpy(e.ipv4, &s->address.sin_addr, sizeof(e.ipv4));
    return e;
}

void kvf_server_stop(kvf_server *s) {
    if (!s)
        return;
    atomic_store_explicit(&s->stopping, 1, memory_order_release);
    for (unsigned i = 0; i < s->started; ++i)
        pthread_join(s->lane[i].thread, NULL);
    if (s->fd >= 0)
        close(s->fd);
    pthread_mutex_destroy(&s->accept_lock);
    free(s);
}
