/*
 * Worker internals. Updates serializes publication RPCs; exports_lock protects
 * read-side export references. Order: updates -> exports_lock -> Trie -> Storage.
 * Peer and prefetch locks never nest with Trie/Storage locks.
 */
#ifndef MINIINFER_REMOTE_INTERNAL_H
#define MINIINFER_REMOTE_INTERNAL_H

#include "miniinfer/remote_cache.h"
#include "remote_sock.h"
#include "../segment_pool.h"
#include <pthread.h>
#include <stdatomic.h>

#define MI_REMOTE_MAX_PEERS 64u

typedef enum {
    EXPORT_EMPTY, EXPORT_PENDING, EXPORT_PUBLISHING, EXPORT_ACTIVE, EXPORT_RETRY, EXPORT_RETIRING, EXPORT_FAILED
} export_state;

typedef struct {
    kvf_manifest manifest; /* Immutable while active, IDs refer to ref's layers. */
    kvf_hash digest;
    mi_page_ref ref;       /* Pins a sealed page until the export stops serving. */
    export_state state;
    kvf_status status;    /* Last result for this revision, protected by updates. */
    int hidden;           /* Serving gate, protected by exports_lock. */
    uint64_t order;       /* Nonzero generation for every installed export. */
} mi_remote_export;

/* A publish RPC may have committed before its acknowledgement reaches us.
 * PUBLISHING and RETRY still require a live Directory lease to serve. */
static inline int mi_remote_export_visible(const mi_remote_export *e) {
    return e && !e->hidden && (e->state == EXPORT_ACTIVE || e->state == EXPORT_PUBLISHING ||
                               e->state == EXPORT_RETRY);
}

static inline int mi_remote_publish_retryable(kvf_status st) {
    return st == KVF_LIMIT || st == KVF_STALE || st == KVF_NOT_FOUND || st == KVF_UNAVAILABLE ||
           st == KVF_IO || st == KVF_TIMEOUT || st == KVF_INTERNAL;
}

typedef struct mi_peer_pool mi_peer_pool;
typedef struct mi_peer_conn mi_peer_conn;
typedef struct mi_prefetch_queue mi_prefetch_queue;
typedef struct mi_remote_server mi_remote_server;

typedef struct {
    void *data;
    size_t capacity;
} mi_batch_buffer;

struct mi_remote_worker {
    mi_tiered_storage *storage;  /* Borrowed, outlives all service threads. */
    mi_paged_kv *cache;          /* Owned, one Trie per namespace. */
    mi_remote_server *server;     /* Owns incoming sockets and accepts up to lanes at once. */
    kvf_client control;          /* Value handle for metadata RPCs. */
    kvf_identity identity;      /* Immutable full namespace. */
    kvf_hash space;              /* Identity digest for fast namespace checks. */
    kvf_worker_info info;        /* Immutable registry identity, endpoint and capacity. */
    mi_location location;        /* Destination tier for newly imported layers. */
    kvf_transport transport_mode; /* Preferred transport selected at startup. */
    int rdma_strict;
    _Atomic kvf_transport active_transport;
    atomic_bool warned_peer_fallback;
    int device;                  /* Storage ordinal or -1 for a CPU-only Worker. */
    uint32_t heartbeat_ms;       /* Period between registry refreshes. */
    uint32_t peer_idle_ms;       /* Accepted idle stream timeout. */
    uint32_t exports_capacity;  /* Upper bound on retained manifests. */
    uint32_t lanes;              /* Number of dedicated inbound slots. */
    pthread_mutex_t updates;     /* Serializes Directory heartbeat and publish/retire. */
    pthread_mutex_t exports_lock; /* Protects server reads of export state/references. */
    pthread_cond_t wake;         /* Wakes heartbeat for new publications or shutdown. */
    pthread_t heartbeat;         /* Must exit before listener and cache teardown. */
    int stopping;                /* Under updates, disables further heartbeat work. */
    atomic_uint_fast64_t local_hits;      /* Sequences with a local cache hit. */
    atomic_uint_fast64_t remote_hits;     /* Sequences with one or more verified imports. */
    atomic_uint_fast64_t reused_tokens;   /* Local plus imported token count. */
    atomic_uint_fast64_t failed_fetches;  /* Attempts ending in an unexpected remote status. */
    atomic_uint_fast64_t evictions;       /* Retired or invalidated export references. */
    atomic_uint active_transfers;        /* Serving requests used for the next load report. */
    mi_remote_export *exports;            /* Fixed table owned for Worker lifetime. */
    uint64_t publication_order;           /* Advanced under updates. */
    mi_segment_pool *segments;            /* Startup slab plus optional DMA resources. */
    mi_peer_pool *peers;                   /* Owned outbound pool, NULL for short mode. */
    mi_prefetch_queue *prefetch;           /* Bounded queue and one joined worker thread. */
    mi_batch_buffer *batch_send;           /* One reusable allocation per inbound lane. */
    mi_batch_buffer batch_recv;            /* Private to the suffix prefetch lane. */
};

kvf_status mi_remote_from_storage(mi_status status);
void mi_remote_serve(mi_remote_worker *w, unsigned lane, mi_remote_socket socket, uint64_t deadline);
kvf_status mi_remote_import(mi_remote_worker *w, mi_kv_sequence *sequence, size_t page,
                             const kvf_grant *grant, uint64_t deadline_ms, const atomic_bool *cancel);
kvf_status mi_remote_receive_batch(mi_remote_worker *w, const kvf_grant *grant, const kvf_batch_page *pages,
                                   uint32_t count, size_t size, uint64_t deadline_ms,
                                   const atomic_bool *cancel, mi_remote_timing *timing);
kvf_status mi_remote_sync_locked(mi_remote_worker *w);
void mi_remote_drop_export(mi_remote_worker *w, mi_remote_export *entry, export_state state);
/* The caller holds exports_lock for handler lookups, or updates for mutations. */
mi_remote_export *mi_remote_find_export(mi_remote_worker *w, kvf_page_key key);
kvf_status mi_remote_resolve_impl(mi_remote_worker *w, const int32_t *tokens, size_t count,
                                   mi_kv_sequence **out, mi_remote_result *result,
                                   const atomic_bool *cancel);
kvf_status mi_remote_resolve_after(mi_remote_worker *w, mi_remote_suffix suffix, const int32_t *tokens,
                                    size_t count, mi_kv_sequence **out, mi_remote_result *result,
                                    const atomic_bool *cancel);
static inline int mi_remote_cancelled(const atomic_bool *cancel) {
    return cancel && atomic_load_explicit(cancel, memory_order_relaxed);
}

/* A zero deadline preserves the legacy per-RPC timeout. */
static inline kvf_status mi_remote_request_client(kvf_client client, uint64_t deadline_ms,
                                                  const atomic_bool *cancel, kvf_client *out) {
    if (mi_remote_cancelled(cancel))
        return KVF_CANCELLED;
    if (deadline_ms) {
        uint64_t now = kvf_now_ms();
        if (now >= deadline_ms)
            return KVF_TIMEOUT;
        if (deadline_ms - now < client.timeout_ms)
            client.timeout_ms = (uint32_t)(deadline_ms - now);
    }
    if (out)
        *out = client;
    return KVF_OK;
}

kvf_status mi_peer_pool_create(unsigned capacity, uint32_t idle_ms, mi_peer_pool **out);
void mi_peer_pool_destroy(mi_peer_pool *pool);
void mi_peer_pool_reap(mi_peer_pool *pool);
kvf_status mi_peer_borrow(mi_peer_pool *pool, const kvf_grant *grant, kvf_transport transport,
                           uint64_t deadline, mi_peer_conn **peer, mi_remote_socket *out, int *reused);
void mi_peer_return(mi_peer_pool *pool, mi_peer_conn *peer, mi_remote_socket socket, int reusable);

kvf_status mi_prefetch_start(mi_remote_worker *w);
void mi_prefetch_stop(mi_remote_worker *w);

#endif
