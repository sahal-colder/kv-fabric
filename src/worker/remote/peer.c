/*
 * Data-plane TCP connection pool. Each checked-out slot has one fd owner.
 * The peer lock protects slot lifetimes. Connect runs after releasing the lock.
 * Neither Directory nor Storage metadata locks enter this module.
 */
#include "internal.h"
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct mi_peer_conn {
    kvf_endpoint endpoint; /* Destination advertised for boot. */
    kvf_uuid boot;         /* A reconnect to a new boot cannot reuse the old fd. */
    int fd;                /* Owned by this slot, -1 when empty or connecting. */
    uint64_t idle_since;   /* Monotonic time at last healthy return. */
    unsigned used;         /* An active importer exclusively owns the fd. */
    unsigned retire;       /* Close on return if a newer boot was observed. */
};

struct mi_peer_pool {
    pthread_mutex_t lock;   /* Protects entry identity and checkout state. */
    unsigned capacity;     /* Maximum simultaneous sockets, including ongoing connects. */
    uint32_t idle_ms;       /* Idle lifetime bounded even when connections are healthy. */
    mi_peer_conn peers[];   /* Owned and closed by this pool. */
};

kvf_status mi_peer_pool_create(unsigned capacity, uint32_t idle_ms, mi_peer_pool **out) {
    if (!capacity)
        return KVF_OK;
    mi_peer_pool *pool = calloc(1, sizeof(*pool) + capacity * sizeof(*pool->peers));
    if (!pool)
        return KVF_LIMIT;
    if (pthread_mutex_init(&pool->lock, NULL) != 0) {
        free(pool);
        return KVF_INTERNAL;
    }
    pool->capacity = capacity;
    pool->idle_ms = idle_ms;
    for (unsigned i = 0; i < capacity; ++i)
        pool->peers[i].fd = -1;
    *out = pool;
    return KVF_OK;
}

static void close_peer(mi_peer_conn *p) {
    if (p->fd >= 0)
        close(p->fd);
    *p = (mi_peer_conn){.fd = -1};
}

void mi_peer_pool_destroy(mi_peer_pool *pool) {
    if (!pool)
        return;
    for (unsigned i = 0; i < pool->capacity; ++i)
        close_peer(&pool->peers[i]);
    pthread_mutex_destroy(&pool->lock);
    free(pool);
}

void mi_peer_pool_reap(mi_peer_pool *pool) {
    if (!pool)
        return;
    uint64_t now = kvf_now_ms();
    pthread_mutex_lock(&pool->lock);
    for (unsigned i = 0; i < pool->capacity; ++i) {
        mi_peer_conn *p = &pool->peers[i];
        if (!p->used && p->fd >= 0 && now - p->idle_since >= pool->idle_ms)
            close_peer(p);
    }
    pthread_mutex_unlock(&pool->lock);
}

kvf_status mi_peer_borrow(mi_peer_pool *pool, const kvf_grant *g, uint64_t deadline,
                           mi_peer_conn **out, int *fd, int *reused) {
    mi_peer_conn *matching = NULL, *available = NULL;
    uint64_t now = kvf_now_ms();
    pthread_mutex_lock(&pool->lock);
    for (unsigned i = 0; i < pool->capacity; ++i) {
        mi_peer_conn *p = &pool->peers[i];
        int same_endpoint = p->endpoint.port == g->endpoint.port &&
            !memcmp(p->endpoint.ipv4, g->endpoint.ipv4, sizeof(p->endpoint.ipv4));
        int same_boot = kvf_uuid_equal(p->boot, g->manifest.owner.boot);
        if (p->used) {
            if (same_endpoint && !same_boot)
                p->retire = 1;
            continue;
        }
        if (p->fd >= 0 && ((same_endpoint && !same_boot) || now - p->idle_since >= pool->idle_ms))
            close_peer(p);
        if (p->fd >= 0 && same_endpoint && same_boot)
            matching = p;
        if (!available || p->fd < 0 || (available->fd >= 0 && p->idle_since < available->idle_since))
            available = p;
    }
    mi_peer_conn *p = matching ? matching : available;
    if (p) {
        if (!matching)
            close_peer(p);
        p->used = 1;
        p->endpoint = g->endpoint;
        p->boot = g->manifest.owner.boot;
    }
    pthread_mutex_unlock(&pool->lock);
    if (!p)
        return KVF_LIMIT;

    if (p->fd >= 0) {
        /* No bytes are expected on an idle stream. Readability includes an orderly EOF. */
        struct pollfd health = {.fd = p->fd, .events = POLLIN};
        if (poll(&health, 1, 0) != 0) {
            close(p->fd);
            p->fd = -1;
        }
    }
    if (reused)
        *reused = p->fd >= 0;
    kvf_status st = p->fd >= 0 ? KVF_OK : kvf_connect(g->endpoint, deadline, &p->fd);
    if (st != KVF_OK) {
        mi_peer_return(pool, p, 0);
        return st;
    }
    *out = p;
    *fd = p->fd;
    return KVF_OK;
}

void mi_peer_return(mi_peer_pool *pool, mi_peer_conn *p, int reusable) {
    if (!p)
        return;
    pthread_mutex_lock(&pool->lock);
    if (!reusable || p->retire) {
        close_peer(p);
    } else {
        p->idle_since = kvf_now_ms();
        p->used = 0;
    }
    pthread_mutex_unlock(&pool->lock);
}
