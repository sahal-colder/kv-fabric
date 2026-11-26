#define _GNU_SOURCE
#include "remote_sock.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#if KVF_HAS_RSOCKETS
#include <rdma/rsocket.h>
#endif

/* rclose() in rdma-core 39 calls a blocking graceful rshutdown(), even
 * for O_NONBLOCK sockets. Its CQ wait has no application deadline. A
 * process-lifetime reaper owns retired sockets; it never borrows a Worker,
 * Storage block or caller buffer (this transport uses rsend/rrecv, no iomap).
 * At most 512 sockets, including queued/in-progress closes, may exist. New
 * opens fail at the bound rather than growing a queue under permanent loss.
 * The OS owns final FD/QP reclamation on process exit. */
#if KVF_HAS_RSOCKETS
#define REMOTE_RDMA_SOCKETS 512u
static pthread_once_t retire_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t retire_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t retire_ready = PTHREAD_COND_INITIALIZER;
static int retire_fds[REMOTE_RDMA_SOCKETS];
static unsigned retire_head, retire_tail, retire_count;
static atomic_uint rdma_live, rdma_retired;
static int retire_started;

static void *retire_worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&retire_lock);
        while (!retire_count) pthread_cond_wait(&retire_ready, &retire_lock);
        int fd = retire_fds[retire_head];
        retire_head = (retire_head + 1) % REMOTE_RDMA_SOCKETS;
        --retire_count;
        pthread_mutex_unlock(&retire_lock);
        rclose(fd);
        atomic_fetch_sub_explicit(&rdma_retired, 1, memory_order_relaxed);
        atomic_fetch_sub_explicit(&rdma_live, 1, memory_order_relaxed);
    }
    return NULL;
}

static void start_retire_worker(void) {
    pthread_t thread;
    if (!pthread_create(&thread, NULL, retire_worker, NULL)) {
        pthread_detach(thread);
        retire_started = 1;
    }
}

static int reserve_rsocket(void) {
    pthread_once(&retire_once, start_retire_worker);
    if (!retire_started) { errno = EAGAIN; return 0; }
    unsigned count = atomic_load_explicit(&rdma_live, memory_order_relaxed);
    while (count < REMOTE_RDMA_SOCKETS) {
        if (atomic_compare_exchange_weak_explicit(&rdma_live, &count, count + 1,
                memory_order_relaxed, memory_order_relaxed)) return 1;
    }
    errno = EMFILE;
    return 0;
}

static void release_unconnected(int fd) {
    if (fd >= 0) rclose(fd); /* No established connection/handshake to drain. */
    atomic_fetch_sub_explicit(&rdma_live, 1, memory_order_relaxed);
}
#endif

unsigned remote_rdma_pending_closes(void) {
#if KVF_HAS_RSOCKETS
    return atomic_load_explicit(&rdma_retired, memory_order_relaxed);
#else
    return 0;
#endif
}

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
    /* Every socket was admitted against rdma_live, so the queue cannot
     * overflow: a currently closing socket also retains its live slot. */
    pthread_mutex_lock(&retire_lock);
    retire_fds[retire_tail] = s.fd;
    retire_tail = (retire_tail + 1) % REMOTE_RDMA_SOCKETS;
    ++retire_count;
    atomic_fetch_add_explicit(&rdma_retired, 1, memory_order_relaxed);
    pthread_cond_signal(&retire_ready);
    pthread_mutex_unlock(&retire_lock);
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
    if (!reserve_rsocket()) return -1;
    int fd = rsocket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { release_unconnected(-1); return -1; }
    if (fd >= 0 && rfcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        int error = errno;
        release_unconnected(fd);
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
            release_unconnected(fd); errno = EINVAL; return -1;
        }
        int size = (int)value;
        if (rsetsockopt(fd, levels[i], options[i], &size, sizeof(size)) < 0 ||
            (i == 0 && rsetsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0)) {
            int error = errno; release_unconnected(fd); errno = error; return -1;
        }
    }
    if (fd >= 0 && getenv("KVF_RDMA_BUFFER_BYTES") && getenv("KVF_PERF_TRACE")) {
        int snd=0, rcv=0, sq=0, rq=0, inl=0;
        socklen_t n=sizeof(int);
        rgetsockopt(fd,SOL_SOCKET,SO_SNDBUF,&snd,&n);
        rgetsockopt(fd,SOL_SOCKET,SO_RCVBUF,&rcv,&n);
        rgetsockopt(fd,SOL_RDMA,RDMA_SQSIZE,&sq,&n);
        rgetsockopt(fd,SOL_RDMA,RDMA_RQSIZE,&rq,&n);
        rgetsockopt(fd,SOL_RDMA,RDMA_INLINE,&inl,&n);
        fprintf(stderr,"KVF_RSOCKET_OPTIONS {\"sndbuf\":%d,\"rcvbuf\":%d,\"sq\":%d,\"rq\":%d,\"inline\":%d}\n",snd,rcv,sq,rq,inl);
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
        if (!reserve_rsocket()) return client;
        client.fd = raccept(listener.fd, NULL, NULL);
        if (client.fd < 0) { release_unconnected(-1); return client; }
        if ( (rfcntl(client.fd, F_SETFL, O_NONBLOCK) >= 0 &&
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

static kvf_status remote_connect_sync(kvf_endpoint endpoint, kvf_transport transport, uint64_t deadline,
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

/* librdmacm/SIW may block inside rconnect/rpoll despite O_NONBLOCK while CM
 * serializes a failed connection. Only this job owns its rsocket until join.
 * Deadline abandonment never frees a borrowed Worker or destination buffer. */
#if KVF_HAS_RSOCKETS
static atomic_uint connect_jobs;
#define REMOTE_CONNECT_JOBS 16u
struct remote_connect_job {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    kvf_endpoint endpoint;
    uint64_t deadline;
    mi_remote_socket socket;
    kvf_status status;
    int done, abandoned;
};
static void free_connect_job(struct remote_connect_job *job) {
    pthread_cond_destroy(&job->ready);
    pthread_mutex_destroy(&job->lock);
    free(job);
    atomic_fetch_sub_explicit(&connect_jobs, 1, memory_order_relaxed);
}
static void *connect_worker(void *context) {
    struct remote_connect_job *job = context;
    job->status = remote_connect_sync(job->endpoint, KVF_TRANSPORT_RDMA_RSOCKET,
                                      job->deadline, &job->socket);
    pthread_mutex_lock(&job->lock);
    job->done = 1;
    int abandoned = job->abandoned;
    pthread_cond_signal(&job->ready);
    pthread_mutex_unlock(&job->lock);
    if (abandoned) {
        if (job->status == KVF_OK) remote_close(job->socket);
        free_connect_job(job);
    }
    return NULL;
}
#endif
kvf_status remote_connect(kvf_endpoint endpoint, kvf_transport transport, uint64_t deadline,
                           mi_remote_socket *out) {
    if (transport == KVF_TRANSPORT_TCP)
        return remote_connect_sync(endpoint, transport, deadline, out);
#if KVF_HAS_RSOCKETS
    if (kvf_now_ms() >= deadline) return KVF_TIMEOUT;
    unsigned n = atomic_load_explicit(&connect_jobs, memory_order_relaxed);
    do {
        if (n >= REMOTE_CONNECT_JOBS) return KVF_LIMIT;
    } while (!atomic_compare_exchange_weak_explicit(&connect_jobs, &n, n + 1,
                memory_order_relaxed, memory_order_relaxed));
    struct remote_connect_job *job = calloc(1, sizeof(*job));
    if (!job) { atomic_fetch_sub(&connect_jobs, 1); return KVF_LIMIT; }
    if (pthread_mutex_init(&job->lock, NULL)) {
        free(job); atomic_fetch_sub(&connect_jobs, 1); return KVF_INTERNAL;
    }
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr)) {
        pthread_mutex_destroy(&job->lock); free(job); atomic_fetch_sub(&connect_jobs, 1);
        return KVF_INTERNAL;
    }
    int error = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!error) error = pthread_cond_init(&job->ready, &attr);
    pthread_condattr_destroy(&attr);
    if (error) {
        pthread_mutex_destroy(&job->lock); free(job); atomic_fetch_sub(&connect_jobs, 1);
        return KVF_INTERNAL;
    }
    job->endpoint = endpoint;
    job->deadline = deadline;
    job->socket.fd = -1;
    pthread_t thread;
    if (pthread_create(&thread, NULL, connect_worker, job)) {
        free_connect_job(job); return KVF_INTERNAL;
    }
    struct timespec until = {.tv_sec = (time_t)(deadline / 1000),
                             .tv_nsec = (long)(deadline % 1000) * 1000000L};
    pthread_mutex_lock(&job->lock);
    while (!job->done && !error)
        error = pthread_cond_timedwait(&job->ready, &job->lock, &until);
    if (!job->done) {
        job->abandoned = 1;
        pthread_mutex_unlock(&job->lock);
        pthread_detach(thread);
        return error == ETIMEDOUT ? KVF_TIMEOUT : KVF_INTERNAL;
    }
    pthread_mutex_unlock(&job->lock);
    pthread_join(thread, NULL);
    kvf_status status = job->status;
    if (status == KVF_OK && kvf_now_ms() >= deadline) {
        remote_close(job->socket); status = KVF_TIMEOUT;
    }
    if (status == KVF_OK) *out = job->socket;
    free_connect_job(job);
    return status;
#else
    (void)endpoint; (void)deadline; (void)out;
    return KVF_UNAVAILABLE;
#endif
}
