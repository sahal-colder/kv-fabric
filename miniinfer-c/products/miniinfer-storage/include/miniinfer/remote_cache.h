/*
 * Remote Connector layer. Worker borrows Storage and owns its Trie, connections
 * and service threads. Sequences and prefetch handles must be released before stop.
 * Lock order is updates -> export table -> Trie -> Storage -> backend metadata.
 */
#ifndef MINIINFER_REMOTE_CACHE_H
#define MINIINFER_REMOTE_CACHE_H

#include "miniinfer/paged_kv.h"
#include "kvfabric/client.h"

typedef struct mi_remote_worker mi_remote_worker;
typedef struct mi_remote_prefetch mi_remote_prefetch;

typedef struct {
    char worker_name[KVF_WORKER_NAME];
    kvf_identity identity;
    kvf_endpoint control, listen, advertise;
    uint32_t lanes, timeout_ms, heartbeat_ms, max_exports;
    uint32_t peer_connections; /* Zero requests short connections, useful as the baseline. */
    uint32_t peer_idle_ms;     /* Upper bound for idle socket retention. */
    mi_location location;
} mi_remote_config;

typedef struct {
    kvf_status remote_status;
    size_t local_tokens, remote_tokens;
} mi_remote_result;

/* Storage is borrowed. The Worker owns its Paged KV and one cache namespace. */
void mi_remote_config_defaults(mi_remote_config *config);
kvf_status mi_remote_worker_start(mi_tiered_storage *storage, mi_remote_config config,
                                    mi_remote_worker **out);
mi_paged_kv *mi_remote_worker_cache(mi_remote_worker *worker);
void mi_remote_worker_info(mi_remote_worker *worker, kvf_worker_info *info);
kvf_endpoint mi_remote_worker_listen_endpoint(const mi_remote_worker *worker);
/* Call after joining local callers and destroying sequences and page leases. */
void mi_remote_worker_stop(mi_remote_worker *worker);

/* Publish only completely committed pages. The sequence belongs to this Worker. */
kvf_status mi_remote_publish(mi_remote_worker *worker, mi_kv_sequence *sequence,
                              const int32_t *tokens, size_t token_count, size_t complete_pages);
/* Append a page, reclaiming an eligible published leaf under capacity pressure. */
kvf_status mi_remote_append_page(mi_remote_worker *worker, mi_kv_sequence *sequence);
/* Close source publication before releasing the cache's prefix references. */
kvf_status mi_remote_evict_prefixes(mi_remote_worker *worker);
/* On remote failure, return the longest already verified prefix for recompute. */
kvf_status mi_remote_resolve(mi_remote_worker *worker, const int32_t *tokens, size_t token_count,
                              mi_kv_sequence **out, mi_remote_result *result);

/* Copies tokens and queues on the worker's bounded prefetch lane. */
kvf_status mi_remote_prefetch_async(mi_remote_worker *worker, const int32_t *tokens, size_t token_count,
                                     mi_remote_prefetch **out);
/* Transfers sequence ownership once. Cancelled work retains only fully verified pages. */
kvf_status mi_remote_prefetch_wait(mi_remote_prefetch *job, mi_kv_sequence **out, mi_remote_result *result);
void mi_remote_prefetch_cancel(mi_remote_prefetch *job);
/* Requests cancellation, joins work on this ticket and releases any unclaimed sequence. */
void mi_remote_prefetch_destroy(mi_remote_prefetch *job);

#endif
