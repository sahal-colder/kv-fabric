/*
 * Worker assembly and registry lifecycle. Worker owns service/heartbeat threads,
 * peer connections, exports and staging, and borrows the Storage object.
 * Order: updates -> exports_lock -> Trie. Registry I/O only under updates.
 */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct mi_remote_server {
    mi_remote_worker *worker;
    kvf_endpoint bound;
    mi_remote_socket listeners[2];
    pthread_mutex_t accept_lock;
    atomic_bool stopping;
    unsigned started, next;
    struct inbound_lane {
        pthread_t thread;
        mi_remote_server *server;
        unsigned index;
    } lanes[];
};

static void *serve(void *arg) {
    struct inbound_lane *lane = arg;
    mi_remote_server *s = lane->server;
    kvf_transport poll_mode = s->worker->transport_mode;
    for (;;) {
        if (atomic_load_explicit(&s->stopping, memory_order_relaxed))
            return NULL;
        struct pollfd fds[] = {
            {.fd = s->listeners[KVF_TRANSPORT_TCP].fd, .events = POLLIN},
            {.fd = s->listeners[KVF_TRANSPORT_RDMA_RSOCKET].fd, .events = POLLIN}
        };
        int ready = 0;
        for (unsigned i = 0; i < 2; ++i) {
            if (fds[i].fd < 0)
                continue;
            int n = remote_poll((kvf_transport)i, &fds[i], 1,
                                poll_mode == KVF_TRANSPORT_TCP ? 100 : 0);
            if (n < 0 && errno != EINTR) {
                ready = -1;
                break;
            }
            if (n > 0)
                ready += n;
        }
        pthread_mutex_lock(&s->accept_lock);
        mi_remote_socket peer = {.fd = -1};
        if (ready > 0) {
            unsigned i = fds[s->next].revents & POLLIN ? s->next : s->next ^ 1u;
            if (fds[i].revents & POLLIN) {
                peer = remote_accept(s->listeners[i]);
                s->next = i ^ 1u;
            }
        }
        pthread_mutex_unlock(&s->accept_lock);
        if (!ready && poll_mode == KVF_TRANSPORT_RDMA_RSOCKET)
            nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
        if (peer.fd >= 0) {
            mi_remote_serve(s->worker, lane->index, peer, kvf_now_ms() + s->worker->control.timeout_ms);
            remote_close(peer);
        } else if (ready != 0) {
            /* Bound retries if a listener reports an error. */
            struct timespec delay = {.tv_nsec = 10000000};
            nanosleep(&delay, NULL);
        }
    }
}

static void stop_server(mi_remote_server *s) {
    if (!s)
        return;
    atomic_store_explicit(&s->stopping, 1, memory_order_relaxed);
    for (unsigned i = 0; i < s->started; ++i)
        pthread_join(s->lanes[i].thread, NULL);
    remote_close(s->listeners[KVF_TRANSPORT_TCP]);
    remote_close(s->listeners[KVF_TRANSPORT_RDMA_RSOCKET]);
    pthread_mutex_destroy(&s->accept_lock);
    free(s);
}

static kvf_status start_server(mi_remote_worker *w, kvf_endpoint listen) {
    mi_remote_server *s = calloc(1, sizeof(*s) + w->lanes * sizeof(*s->lanes));
    if (!s)
        return KVF_LIMIT;
    if (pthread_mutex_init(&s->accept_lock, NULL) != 0) {
        free(s);
        return KVF_INTERNAL;
    }
    s->worker = w;
    s->bound = listen;
    s->listeners[KVF_TRANSPORT_TCP].fd = -1;
    s->listeners[KVF_TRANSPORT_RDMA_RSOCKET].fd = -1;
    atomic_init(&s->stopping, 0);

    kvf_status st;
    if (w->transport_mode == KVF_TRANSPORT_TCP || !w->rdma_strict) {
        st = remote_listen(&s->bound, w->lanes, KVF_TRANSPORT_TCP, &s->listeners[KVF_TRANSPORT_TCP]);
        if (st != KVF_OK)
            goto failed;
    }
    if (w->transport_mode == KVF_TRANSPORT_RDMA_RSOCKET) {
        const char *reason = remote_rdma_unavailable();
        /* IB/RoCE CM and TCP have separate port spaces. Reuse the TCP port when fallback is enabled. */
        if (!reason && remote_listen(&s->bound, w->lanes, KVF_TRANSPORT_RDMA_RSOCKET,
                                      &s->listeners[KVF_TRANSPORT_RDMA_RSOCKET]) != KVF_OK)
            reason = strerror(errno);
        if (reason) {
            if (w->rdma_strict) {
                fprintf(stderr, "kv-fabric: RDMA_RSOCKET unavailable (%s). TCP fallback is disabled.\n", reason);
                st = KVF_UNAVAILABLE;
                goto failed;
            }
            fprintf(stderr, "kv-fabric: warning: RDMA_RSOCKET unavailable (%s). Falling back to TCP.\n", reason);
            w->transport_mode = KVF_TRANSPORT_TCP;
        }
    }
    atomic_init(&w->active_transport, w->transport_mode);

    st = KVF_INTERNAL;
    for (unsigned i = 0; i < w->lanes; ++i) {
        s->lanes[i].server = s;
        s->lanes[i].index = i;
        if (pthread_create(&s->lanes[i].thread, NULL, serve, &s->lanes[i]) != 0)
            goto failed;
        ++s->started;
    }
    w->server = s;
    return KVF_OK;

failed:
    stop_server(s);
    return st;
}

kvf_status mi_remote_transport_parse(const char *name, kvf_transport *out) {
    if (!name || !out)
        return KVF_INVALID;
    if (strcmp(name, "tcp") == 0) {
        *out = KVF_TRANSPORT_TCP;
        return KVF_OK;
    }
    if (strcmp(name, "rdma") == 0) {
        *out = KVF_TRANSPORT_RDMA_RSOCKET;
        return KVF_OK;
    }
    return KVF_INVALID;
}

const char *mi_remote_transport_name(kvf_transport transport) {
    return transport == KVF_TRANSPORT_RDMA_RSOCKET ? "RDMA_RSOCKET" : "TCP";
}

void mi_remote_config_defaults(mi_remote_config *cfg) {
    *cfg = (mi_remote_config){
        .control = {{127, 0, 0, 1}, 9000}, .listen = {{127, 0, 0, 1}, 0},
        .lanes = 8, .timeout_ms = KVF_DEFAULT_TIMEOUT_MS, .heartbeat_ms = 1000, .max_exports = 1024,
        .peer_connections = 8, .peer_idle_ms = 500, .transport_mode = KVF_TRANSPORT_TCP
    };
}

kvf_status mi_remote_from_storage(mi_status st) {
    switch (st) {
    case MI_OK: return KVF_OK;
    case MI_EINVAL: return KVF_INVALID;
    case MI_ENOMEM: return KVF_LIMIT;
    case MI_ENOENT: return KVF_STALE;
    case MI_ECRC: return KVF_CRC;
    case MI_EOVERFLOW:
    case MI_EINTERNAL: return KVF_INTERNAL;
    default: return KVF_UNAVAILABLE;
    }
}

mi_remote_export *mi_remote_find_export(mi_remote_worker *w, kvf_page_key key) {
    for (uint32_t i = 0; i < w->exports_capacity; ++i)
        if (w->exports[i].state != EXPORT_EMPTY &&
            kvf_page_key_equal(w->exports[i].manifest.key, key))
            return &w->exports[i];
    return NULL;
}

void mi_remote_worker_info(mi_remote_worker *w, kvf_worker_info *info) {
    *info = w->info;
    info->local_hits = atomic_load_explicit(&w->local_hits, memory_order_relaxed);
    info->remote_hits = atomic_load_explicit(&w->remote_hits, memory_order_relaxed);
    info->reused_tokens = atomic_load_explicit(&w->reused_tokens, memory_order_relaxed);
    info->failed_fetches = atomic_load_explicit(&w->failed_fetches, memory_order_relaxed);
    info->active_transfers = atomic_load_explicit(&w->active_transfers, memory_order_relaxed);
    pthread_mutex_lock(&w->exports_lock);
    for (uint32_t i = 0; i < w->exports_capacity; ++i)
        if (w->exports[i].state == EXPORT_ACTIVE && !w->exports[i].hidden)
            ++info->published_pages;
    pthread_mutex_unlock(&w->exports_lock);
}

/* Control RPCs are serialized per worker. A restarted directory is replayed
 * only after registration with this boot, never by replacing an observed boot. */
kvf_status mi_remote_sync_locked(mi_remote_worker *w) {
    kvf_worker_info info;
    mi_remote_worker_info(w, &info);
    kvf_status st = kvf_client_heartbeat(w->control, &info);
    if (st == KVF_NOT_FOUND) {
        st = kvf_client_register(w->control, &info, (kvf_uuid){0});
        if (st == KVF_OK) {
            pthread_mutex_lock(&w->exports_lock);
            for (uint32_t i = 0; i < w->exports_capacity; ++i) {
                if (w->exports[i].state == EXPORT_ACTIVE)
                    w->exports[i].state = EXPORT_RETRY;
            }
            pthread_mutex_unlock(&w->exports_lock);
        }
    }
    if (st != KVF_OK)
        return st;

    kvf_status pending = KVF_OK;
    for (uint32_t i = 0; i < w->exports_capacity; ++i) {
        mi_remote_export *e = &w->exports[i];
        if (e->state == EXPORT_RETIRING) {
            st = kvf_client_invalidate(w->control, &e->manifest);
            if (st != KVF_OK && st != KVF_NOT_FOUND && st != KVF_STALE)
                return st;
            mi_remote_drop_export(w, e, EXPORT_EMPTY);
        }
        if (e->state == EXPORT_PENDING || e->state == EXPORT_RETRY) {
            pthread_mutex_lock(&w->exports_lock);
            int hidden = e->hidden;
            if (!hidden)
                e->state = EXPORT_PUBLISHING;
            pthread_mutex_unlock(&w->exports_lock);
            if (hidden) {
                pthread_mutex_lock(&w->exports_lock);
                e->state = EXPORT_RETIRING;
                pthread_mutex_unlock(&w->exports_lock);
                continue;
            }
            st = kvf_client_publish(w->control, &e->manifest);
            e->status = st;
            if (st != KVF_OK && !mi_remote_publish_retryable(st)) {
                mi_remote_drop_export(w, e, EXPORT_FAILED);
            } else {
                pthread_mutex_lock(&w->exports_lock);
                e->state = st == KVF_OK ? EXPORT_ACTIVE : EXPORT_RETRY;
                pthread_mutex_unlock(&w->exports_lock);
            }
            if (st != KVF_OK && pending == KVF_OK)
                pending = st;
            if (st == KVF_IO || st == KVF_TIMEOUT || st == KVF_STALE)
                break;
        }
    }
    return pending;
}

static void *heartbeat(void *context) {
    mi_remote_worker *w = context;
    pthread_mutex_lock(&w->updates);
    while (!w->stopping) {
        uint64_t end = kvf_now_ms() + w->heartbeat_ms;
        struct timespec deadline = {.tv_sec = (time_t)(end / 1000),
                                    .tv_nsec = (long)(end % 1000) * 1000000L};
        int error = pthread_cond_timedwait(&w->wake, &w->updates, &deadline);
        if (error && error != ETIMEDOUT)
            break;
        if (w->stopping)
            break;
        (void)mi_remote_sync_locked(w);
        pthread_mutex_unlock(&w->updates);
        mi_peer_pool_reap(w->peers);
        pthread_mutex_lock(&w->updates);
    }
    pthread_mutex_unlock(&w->updates);
    return NULL;
}

static int specified(kvf_endpoint e) {
    return e.ipv4[0] || e.ipv4[1] || e.ipv4[2] || e.ipv4[3];
}

/* Both service threads must be stopped before releasing the storage they borrow. */
static void release_worker(mi_remote_worker *w) {
    if (w->exports)
        for (uint32_t i = 0; i < w->exports_capacity; ++i)
            mi_page_ref_release(&w->exports[i].ref);
    mi_paged_kv_destroy(w->cache);
    mi_peer_pool_destroy(w->peers);
    mi_segment_pool_destroy(w->segments);
    if (w->batch_send) {
        for (unsigned i = 0; i < w->lanes; ++i)
            if (w->batch_send[i].data)
                (void)mi_device_host_free(w->device, w->batch_send[i].data);
    }
    free(w->batch_send);
    free(w->batch_recv.data);
    free(w->exports);
    free(w);
}

kvf_status mi_remote_worker_start(mi_tiered_storage *storage, mi_remote_config cfg,
                                    mi_remote_worker **out) {
    kvf_hash space;
    if (!storage || !out || !cfg.control.port || !specified(cfg.control) ||
        (!specified(cfg.listen) && !specified(cfg.advertise)) ||
        (unsigned)cfg.location > MI_LOCATION_GPU ||
        (unsigned)cfg.transport_mode > KVF_TRANSPORT_RDMA_RSOCKET ||
        !cfg.lanes || cfg.lanes > 64 || !cfg.timeout_ms || cfg.timeout_ms > 30000 ||
        cfg.peer_connections > MI_REMOTE_MAX_PEERS ||
        !cfg.heartbeat_ms || cfg.heartbeat_ms > 30000 || !cfg.max_exports || cfg.max_exports > 65536 ||
        kvf_identity_hash(&cfg.identity, &space) != KVF_OK)
        return KVF_INVALID;
    size_t pages = mi_storage_block_capacity(storage) / cfg.identity.layers;
    if (!pages)
        return KVF_LIMIT;
    int device = mi_storage_gpu_device(storage);
    if (cfg.location == MI_LOCATION_GPU && device < 0)
        return KVF_UNAVAILABLE;

    mi_remote_worker *w = calloc(1, sizeof(*w));
    if (!w)
        return KVF_LIMIT;
    w->storage = storage;
    w->control = (kvf_client){cfg.control, cfg.timeout_ms};
    w->identity = cfg.identity;
    w->space = space;
    w->device = device;
    w->location = cfg.location;
    w->transport_mode = cfg.transport_mode;
    w->rdma_strict = cfg.rdma_strict;
    w->lanes = cfg.lanes;
    w->heartbeat_ms = cfg.heartbeat_ms;
    w->peer_idle_ms = cfg.peer_idle_ms ? cfg.peer_idle_ms : 500;
    if (w->peer_idle_ms > cfg.timeout_ms)
        w->peer_idle_ms = cfg.timeout_ms;
    w->exports_capacity = pages < cfg.max_exports ? (uint32_t)pages : cfg.max_exports;
    w->info.capacity_pages = w->exports_capacity;
    memcpy(w->info.owner.name, cfg.worker_name, sizeof(w->info.owner.name));
    atomic_init(&w->local_hits, 0);
    atomic_init(&w->remote_hits, 0);
    atomic_init(&w->reused_tokens, 0);
    atomic_init(&w->failed_fetches, 0);
    atomic_init(&w->evictions, 0);
    atomic_init(&w->active_transfers, 0);
    atomic_init(&w->warned_peer_fallback, 0);

    kvf_status st = kvf_uuid_new(&w->info.owner.boot);
    if (st != KVF_OK)
        goto failed;
    if (!kvf_owner_valid(&w->info.owner)) {
        st = KVF_INVALID;
        goto failed;
    }
    char namespace_hex[65];
    kvf_hash_format(space, namespace_hex);
    st = mi_remote_from_storage(mi_paged_kv_create(storage, cfg.identity.layers, cfg.identity.tokens_per_page,
                                                   cfg.identity.bytes_per_layer, namespace_hex, &w->cache));
    if (st != KVF_OK)
        goto failed;

    st = KVF_LIMIT;
    w->exports = calloc(w->exports_capacity, sizeof(*w->exports));
    w->batch_send = calloc(w->lanes, sizeof(*w->batch_send));
    if (!w->exports || !w->batch_send)
        goto failed;
    /* One slot per incoming lane, a separate publication slot and two per GPU importer. */
    unsigned dma_stages = cfg.identity.layers > 1 && cfg.peer_connections ? 2 : 1;
    unsigned segments = w->lanes + 1 + (cfg.location == MI_LOCATION_GPU ? dma_stages * w->lanes : 0);
    st = mi_remote_from_storage(mi_segment_pool_create(device, cfg.identity.bytes_per_layer,
                                                        segments, 1, &w->segments));
    if (st != KVF_OK)
        goto failed;
    st = mi_peer_pool_create(cfg.peer_connections, w->peer_idle_ms, &w->peers);
    if (st != KVF_OK)
        goto failed;

    st = KVF_INTERNAL;
    if (pthread_mutex_init(&w->updates, NULL) != 0)
        goto failed;
    if (pthread_mutex_init(&w->exports_lock, NULL) != 0)
        goto updates_ready;
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0)
        goto locks_ready;
    int error = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!error)
        error = pthread_cond_init(&w->wake, &attr);
    pthread_condattr_destroy(&attr);
    if (error)
        goto locks_ready;

    st = mi_prefetch_start(w);
    if (st != KVF_OK)
        goto condition_ready;
    st = start_server(w, cfg.listen);
    if (st != KVF_OK)
        goto condition_ready;
    kvf_endpoint bound = w->server->bound;
    w->info.endpoint = specified(cfg.advertise) ? cfg.advertise : bound;
    if (!w->info.endpoint.port)
        w->info.endpoint.port = bound.port;

    kvf_worker_info previous;
    int online;
    st = kvf_client_worker_status(w->control, w->info.owner.name, &previous, &online);
    if (st == KVF_OK || st == KVF_NOT_FOUND)
        st = kvf_client_register(w->control, &w->info, st == KVF_OK ? previous.owner.boot : (kvf_uuid){0});
    if (pthread_create(&w->heartbeat, NULL, heartbeat, w) != 0) {
        if (st == KVF_OK)
            (void)kvf_client_unregister(w->control, &w->info.owner);
        st = KVF_INTERNAL;
        goto condition_ready;
    }
    *out = w;
    return KVF_OK;

condition_ready:
    mi_prefetch_stop(w);
    stop_server(w->server);
    pthread_cond_destroy(&w->wake);
locks_ready:
    pthread_mutex_destroy(&w->exports_lock);
updates_ready:
    pthread_mutex_destroy(&w->updates);
failed:
    release_worker(w);
    return st;
}

mi_paged_kv *mi_remote_worker_cache(mi_remote_worker *w) {
    return w->cache;
}

kvf_endpoint mi_remote_worker_listen_endpoint(const mi_remote_worker *w) {
    return w->server->bound;
}

kvf_transport mi_remote_worker_active_transport(const mi_remote_worker *w) {
    return atomic_load_explicit(&w->active_transport, memory_order_relaxed);
}

void mi_remote_worker_stop(mi_remote_worker *w) {
    if (!w)
        return;
    mi_prefetch_stop(w);
    pthread_mutex_lock(&w->updates);
    w->stopping = 1;
    pthread_cond_signal(&w->wake);
    pthread_mutex_unlock(&w->updates);
    pthread_join(w->heartbeat, NULL);
    stop_server(w->server);
    (void)kvf_client_unregister(w->control, &w->info.owner);
    pthread_cond_destroy(&w->wake);
    pthread_mutex_destroy(&w->exports_lock);
    pthread_mutex_destroy(&w->updates);
    release_worker(w);
}
