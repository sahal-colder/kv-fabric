/*
 * Global metadata API. Directory owns registry, manifest replicas and leases.
 * A single internal lock protects them. Callers receive value copies and do I/O after return.
 */
#ifndef KVFABRIC_DIRECTORY_H
#define KVFABRIC_DIRECTORY_H

#include "kvfabric.h"

typedef struct kvf_directory kvf_directory;
typedef struct kvf_server kvf_server;

typedef struct {
    uint32_t max_pages, max_workers, max_leases;
    uint32_t worker_ttl_ms, lease_ttl_ms;
} kvf_directory_config;

typedef struct {
    uint32_t pages, workers, leases;
    uint64_t publishes, duplicates, lookups, matched_pages, invalidations;
} kvf_directory_stats;

void kvf_directory_defaults(kvf_directory_config *config);
kvf_status kvf_directory_create(kvf_directory_config config, kvf_directory **out);
void kvf_directory_destroy(kvf_directory *directory);
kvf_status kvf_directory_register(kvf_directory *d, const kvf_worker_info *worker, kvf_uuid replaces);
kvf_status kvf_directory_heartbeat(kvf_directory *d, const kvf_worker_info *worker);
kvf_status kvf_directory_unregister(kvf_directory *d, const kvf_owner *owner);
kvf_status kvf_directory_worker_status(kvf_directory *d, const char *name, kvf_worker_info *out,
                                        int *online);
kvf_status kvf_directory_publish(kvf_directory *d, const kvf_manifest *manifest);
kvf_status kvf_directory_lookup_prefix(kvf_directory *d, kvf_hash space, const kvf_hash *pages,
                                        uint32_t count, uint32_t *matched);
kvf_status kvf_directory_lookup_page(kvf_directory *d, kvf_page_key key, kvf_manifest *out,
                                      kvf_endpoint *endpoint);
kvf_status kvf_directory_acquire(kvf_directory *d, kvf_page_key key, kvf_grant *out);
kvf_status kvf_directory_acquire_prefix(kvf_directory *d, kvf_hash space, const kvf_hash *pages,
                                        uint32_t count, kvf_grant *out);
kvf_status kvf_directory_renew(kvf_directory *d, kvf_ticket ticket, uint32_t *ttl_ms);
kvf_status kvf_directory_release(kvf_directory *d, kvf_ticket ticket);
kvf_status kvf_directory_validate(kvf_directory *d, const kvf_fetch_request *request);
kvf_status kvf_directory_invalidate(kvf_directory *d, const kvf_manifest *manifest);
/* Conditional pressure eviction. Administrative invalidation may revoke active leases. */
kvf_status kvf_directory_retire(kvf_directory *d, const kvf_manifest *manifest);
void kvf_directory_get_stats(kvf_directory *d, kvf_directory_stats *out);

/* Stop the listener before destroying its borrowed directory. */
kvf_status kvf_control_start(kvf_directory *d, kvf_endpoint listen, unsigned lanes,
                              uint32_t timeout_ms, kvf_server **out);

#endif
