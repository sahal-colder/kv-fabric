/* C client layer. Clients own no connections. Calls borrow input/output buffers and take no locks. */
#ifndef KVFABRIC_CLIENT_H
#define KVFABRIC_CLIENT_H

#include "directory.h"

typedef struct {
    kvf_endpoint endpoint;
    uint32_t timeout_ms;
} kvf_client;

kvf_status kvf_client_register(kvf_client c, const kvf_worker_info *worker, kvf_uuid replaces);
kvf_status kvf_client_heartbeat(kvf_client c, const kvf_worker_info *worker);
kvf_status kvf_client_unregister(kvf_client c, const kvf_owner *owner);
kvf_status kvf_client_worker_status(kvf_client c, const char *name, kvf_worker_info *out, int *online);
kvf_status kvf_client_publish(kvf_client c, const kvf_manifest *manifest);
kvf_status kvf_client_lookup_prefix(kvf_client c, kvf_hash space, const kvf_hash *pages,
                                     uint32_t count, uint32_t *matched);
kvf_status kvf_client_lookup_page(kvf_client c, kvf_page_key key, kvf_manifest *out, kvf_endpoint *endpoint);
kvf_status kvf_client_acquire(kvf_client c, kvf_page_key key, kvf_grant *out);
kvf_status kvf_client_acquire_prefix(kvf_client c, kvf_hash space, const kvf_hash *pages,
                                     uint32_t count, kvf_grant *out);
kvf_status kvf_client_renew(kvf_client c, kvf_ticket ticket, uint32_t *ttl_ms);
kvf_status kvf_client_release(kvf_client c, kvf_ticket ticket);
kvf_status kvf_client_validate(kvf_client c, const kvf_fetch_request *request);
kvf_status kvf_client_invalidate(kvf_client c, const kvf_manifest *manifest);
kvf_status kvf_client_retire(kvf_client c, const kvf_manifest *manifest);
kvf_status kvf_client_stats(kvf_client c, kvf_directory_stats *out);

kvf_status kvf_client_fetch(kvf_endpoint source, uint32_t timeout_ms,
                             const kvf_fetch_request *request, void *payload, size_t capacity);
kvf_status kvf_client_fetch_on(int fd, uint64_t deadline, const kvf_fetch_request *request,
                                void *payload, size_t capacity);
kvf_status kvf_client_block_meta(kvf_endpoint source, uint32_t timeout_ms,
                                  const kvf_fetch_request *request, kvf_layer *out);

#endif
