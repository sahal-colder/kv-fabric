/* SDK client. RPCs own stack frames, borrow user buffers, and take no shared metadata locks. */
#define _POSIX_C_SOURCE 200809L
#include "kvfabric/client.h"
#include "kvfabric/wire.h"
#include <string.h>

static kvf_status call(kvf_client c, kvf_opcode op, const kvf_writer *request,
                        void *reply, size_t capacity, size_t *size) {
    return kvf_rpc(c.endpoint, op, request, reply, capacity, size, c.timeout_ms);
}

static kvf_status worker_call(kvf_client c, kvf_opcode op, const kvf_worker_info *info, kvf_uuid replaces) {
    uint8_t bytes[160];
    kvf_writer w = {.buffer = bytes, .capacity = sizeof(bytes)};
    kvf_put_worker(&w, info);
    if (op == KVF_REGISTER)
        kvf_put_bytes(&w, replaces.bytes, 16);
    return call(c, op, &w, NULL, 0, NULL);
}

kvf_status kvf_client_register(kvf_client c, const kvf_worker_info *info, kvf_uuid replaces) {
    return worker_call(c, KVF_REGISTER, info, replaces);
}

kvf_status kvf_client_heartbeat(kvf_client c, const kvf_worker_info *info) {
    return worker_call(c, KVF_HEARTBEAT, info, (kvf_uuid){0});
}

kvf_status kvf_client_unregister(kvf_client c, const kvf_owner *owner) {
    uint8_t bytes[80];
    kvf_writer w = {.buffer = bytes, .capacity = sizeof(bytes)};
    kvf_put_owner(&w, owner);
    return call(c, KVF_UNREGISTER, &w, NULL, 0, NULL);
}

kvf_status kvf_client_worker_status(kvf_client c, const char *name, kvf_worker_info *out, int *online) {
    if (!name || !out || !online)
        return KVF_INVALID;
    size_t len = strnlen(name, KVF_WORKER_NAME);
    if (!len || len >= KVF_WORKER_NAME)
        return KVF_INVALID;
    uint8_t bytes[160];
    kvf_writer w = {.buffer = bytes, .capacity = sizeof(bytes)};
    kvf_put_u16(&w, (uint16_t)len);
    kvf_put_bytes(&w, name, len);
    size_t n;
    kvf_status st = call(c, KVF_WORKER_STATUS, &w, bytes, sizeof(bytes), &n);
    if (st != KVF_OK)
        return st;
    kvf_reader r = {.buffer = bytes, .size = n};
    uint16_t active = kvf_get_u16(&r);
    kvf_get_worker(&r, out);
    if (!kvf_reader_done(&r) || active > 1 || strcmp(name, out->owner.name))
        return KVF_PROTOCOL;
    *online = active;
    return KVF_OK;
}

static kvf_status manifest_call(kvf_client c, kvf_opcode op, const kvf_manifest *m) {
    uint8_t bytes[KVF_METADATA_BYTES];
    kvf_writer w = {.buffer = bytes, .capacity = sizeof(bytes)};
    kvf_put_manifest(&w, m);
    return call(c, op, &w, NULL, 0, NULL);
}

kvf_status kvf_client_publish(kvf_client c, const kvf_manifest *m) {
    return manifest_call(c, KVF_PUBLISH, m);
}

kvf_status kvf_client_invalidate(kvf_client c, const kvf_manifest *m) {
    return manifest_call(c, KVF_INVALIDATE, m);
}

kvf_status kvf_client_retire(kvf_client c, const kvf_manifest *m) {
    return manifest_call(c, KVF_RETIRE, m);
}

static void put_candidates(kvf_writer *w, kvf_hash space, const kvf_hash *pages, uint32_t count) {
    kvf_put_bytes(w, space.bytes, 32);
    kvf_put_u32(w, count);
    for (uint32_t i = 0; i < count; ++i)
        kvf_put_bytes(w, pages[i].bytes, 32);
}

kvf_status kvf_client_lookup_prefix(kvf_client c, kvf_hash space, const kvf_hash *pages,
                                     uint32_t count, uint32_t *matched) {
    if (!pages || !count || count > KVF_MAX_LOOKUP || !matched)
        return KVF_INVALID;
    uint8_t bytes[36 + 32 * KVF_MAX_LOOKUP];
    kvf_writer w = {.buffer = bytes, .capacity = sizeof(bytes)};
    put_candidates(&w, space, pages, count);
    size_t n;
    kvf_status st = call(c, KVF_LOOKUP_PREFIX, &w, bytes, 4, &n);
    if (st != KVF_OK)
        return st;
    kvf_reader r = {.buffer = bytes, .size = n};
    *matched = kvf_get_u32(&r);
    return kvf_reader_done(&r) && *matched <= count ? KVF_OK : KVF_PROTOCOL;
}

static int get_grant(kvf_reader *r, kvf_page_key key, int leased, kvf_grant *out) {
    kvf_get_manifest(r, &out->manifest);
    kvf_get_endpoint(r, &out->endpoint);
    if (leased) {
        kvf_get_ticket(r, &out->ticket);
        out->ttl_ms = kvf_get_u32(r);
        if (!out->ttl_ms || !out->ticket.fence || kvf_uuid_zero(out->ticket.directory) ||
            kvf_uuid_zero(out->ticket.secret))
            return 0;
    }
    return !r->failed && out->endpoint.port && kvf_page_key_equal(key, out->manifest.key) &&
        kvf_manifest_validate(&out->manifest) == KVF_OK;
}

static kvf_status page_call(kvf_client c, kvf_opcode op, kvf_page_key key, const kvf_hash *pages,
                             uint32_t count, kvf_grant *out) {
    if (!out)
        return KVF_INVALID;
    uint8_t bytes[KVF_METADATA_BYTES];
    kvf_writer w = {.buffer = bytes, .capacity = sizeof(bytes)};
    if (op == KVF_ACQUIRE_PREFIX)
        put_candidates(&w, key.space, pages, count);
    else
        kvf_put_key(&w, key);
    size_t n;
    kvf_status st = call(c, op, &w, bytes, sizeof(bytes), &n);
    if (st != KVF_OK)
        return st;
    kvf_reader r = {.buffer = bytes, .size = n};
    return get_grant(&r, key, op != KVF_LOOKUP_PAGE, out) && kvf_reader_done(&r) ? KVF_OK : KVF_PROTOCOL;
}

kvf_status kvf_client_lookup_page(kvf_client c, kvf_page_key key, kvf_manifest *out, kvf_endpoint *endpoint) {
    if (!out || !endpoint)
        return KVF_INVALID;
    kvf_grant g;
    kvf_status st = page_call(c, KVF_LOOKUP_PAGE, key, NULL, 0, &g);
    if (st == KVF_OK) {
        *out = g.manifest;
        *endpoint = g.endpoint;
    }
    return st;
}

kvf_status kvf_client_acquire(kvf_client c, kvf_page_key key, kvf_grant *out) {
    return page_call(c, KVF_ACQUIRE, key, NULL, 0, out);
}

kvf_status kvf_client_acquire_prefix(kvf_client c, kvf_hash space, const kvf_hash *pages,
                                     uint32_t count, kvf_grant *out) {
    if (!pages || !count || count > KVF_MAX_LOOKUP || !out)
        return KVF_INVALID;
    return page_call(c, KVF_ACQUIRE_PREFIX, (kvf_page_key){space, pages[0]}, pages, count, out);
}

kvf_status kvf_client_acquire_batch(kvf_client c, kvf_hash space, const kvf_hash *pages, uint32_t count,
                                    kvf_grant *grants, uint32_t *matched) {
    if (!pages || !count || count > KVF_MAX_BATCH_PAGES || !grants || !matched)
        return KVF_INVALID;
    uint8_t bytes[KVF_BATCH_GRANT_BYTES];
    kvf_writer w = {.buffer = bytes, .capacity = sizeof(bytes)};
    put_candidates(&w, space, pages, count);
    size_t n;
    kvf_status st = call(c, KVF_ACQUIRE_BATCH, &w, bytes, sizeof(bytes), &n);
    if (st != KVF_OK)
        return st;
    kvf_reader r = {.buffer = bytes, .size = n};
    uint32_t hits = kvf_get_u32(&r);
    if (!hits || hits > count)
        return KVF_PROTOCOL;
    for (uint32_t i = 0; i < hits; ++i)
        if (!get_grant(&r, (kvf_page_key){space, pages[i]}, 1, &grants[i]))
            return KVF_PROTOCOL;
    if (!kvf_reader_done(&r))
        return KVF_PROTOCOL;
    *matched = hits;
    return KVF_OK;
}

static kvf_status check_batch(kvf_client c, int fd, uint64_t deadline, const kvf_batch_page *pages,
                               uint32_t count, int finish) {
    uint8_t bytes[4 + KVF_MAX_BATCH_PAGES * 156];
    kvf_writer w = {.buffer = bytes, .capacity = sizeof(bytes)};
    kvf_put_batch(&w, pages, count);
    kvf_opcode op = finish ? KVF_FINISH_BATCH : KVF_VALIDATE_BATCH;
    return fd >= 0 ? kvf_rpc_on(fd, op, &w, NULL, 0, NULL, deadline) : call(c, op, &w, NULL, 0, NULL);
}

kvf_status kvf_client_check_batch(kvf_client c, const kvf_batch_page *pages, uint32_t count, int finish) {
    return check_batch(c, -1, 0, pages, count, finish);
}

kvf_status kvf_client_validate_batch_on(int fd, uint64_t deadline, const kvf_batch_page *pages, uint32_t count) {
    return check_batch((kvf_client){0}, fd, deadline, pages, count, 0);
}

static kvf_status ticket_call(kvf_client c, kvf_opcode op, kvf_ticket ticket, uint32_t *ttl_ms) {
    uint8_t bytes[44];
    kvf_writer w = {.buffer = bytes, .capacity = sizeof(bytes)};
    kvf_put_ticket(&w, ticket);
    size_t n;
    kvf_status st = call(c, op, &w, bytes, ttl_ms ? 4 : 0, &n);
    if (st != KVF_OK || !ttl_ms)
        return st;
    kvf_reader r = {.buffer = bytes, .size = n};
    *ttl_ms = kvf_get_u32(&r);
    return kvf_reader_done(&r) && *ttl_ms ? KVF_OK : KVF_PROTOCOL;
}

kvf_status kvf_client_renew(kvf_client c, kvf_ticket ticket, uint32_t *ttl_ms) {
    return ticket_call(c, KVF_RENEW, ticket, ttl_ms);
}

kvf_status kvf_client_release(kvf_client c, kvf_ticket ticket) {
    return ticket_call(c, KVF_RELEASE, ticket, NULL);
}

static kvf_status fetch_call(kvf_client c, kvf_opcode op, const kvf_fetch_request *request,
                              void *reply, size_t capacity, size_t *n) {
    uint8_t bytes[156];
    kvf_writer w = {.buffer = bytes, .capacity = sizeof(bytes)};
    kvf_put_fetch(&w, request);
    return call(c, op, &w, reply, capacity, n);
}

kvf_status kvf_client_validate(kvf_client c, const kvf_fetch_request *request) {
    return fetch_call(c, KVF_VALIDATE, request, NULL, 0, NULL);
}

static kvf_status fetch(kvf_client c, int fd, uint64_t deadline, const kvf_fetch_request *r,
                         void *payload, size_t capacity) {
    if (!r->layer.size || r->layer.size > capacity || r->layer.size > KVF_MAX_LAYER_BYTES)
        return KVF_INVALID;
    size_t n;
    kvf_status st;
    if (fd >= 0) {
        uint8_t bytes[156];
        kvf_writer w = {.buffer = bytes, .capacity = sizeof(bytes)};
        kvf_put_fetch(&w, r);
        st = kvf_rpc_on(fd, KVF_FETCH_STREAM, &w, payload, r->layer.size, &n, deadline);
    } else {
        st = fetch_call(c, KVF_FETCH, r, payload, r->layer.size, &n);
    }
    if (st != KVF_OK)
        return st;
    if (n != r->layer.size)
        return KVF_PROTOCOL;
    return kvf_crc32c(payload, n) == r->layer.checksum ? KVF_OK : KVF_CRC;
}

kvf_status kvf_client_fetch(kvf_endpoint ep, uint32_t timeout_ms, const kvf_fetch_request *r,
                             void *payload, size_t capacity) {
    return fetch((kvf_client){ep, timeout_ms}, -1, 0, r, payload, capacity);
}

kvf_status kvf_client_fetch_on(int fd, uint64_t deadline, const kvf_fetch_request *r,
                                void *payload, size_t capacity) {
    return fetch((kvf_client){0}, fd, deadline, r, payload, capacity);
}

kvf_status kvf_client_fetch_batch_on(int fd, uint64_t deadline, const kvf_batch_page *pages,
                                      uint32_t count, void *payload, size_t size, kvf_receive_timing *timing) {
    if (!payload || !size || size > KVF_MAX_BATCH_BYTES)
        return KVF_INVALID;
    uint8_t bytes[4 + KVF_MAX_BATCH_PAGES * 156];
    kvf_writer w = {.buffer = bytes, .capacity = sizeof(bytes)};
    kvf_put_batch(&w, pages, count);
    size_t n;
    kvf_status st = kvf_rpc_on_timed(fd, KVF_FETCH_BATCH, &w, payload, size, &n, deadline, timing);
    return st == KVF_OK && n != size ? KVF_PROTOCOL : st;
}

kvf_status kvf_client_block_meta(kvf_endpoint ep, uint32_t timeout_ms, const kvf_fetch_request *req,
                                  kvf_layer *out) {
    uint8_t reply[28];
    size_t n;
    kvf_status st = fetch_call((kvf_client){ep, timeout_ms}, KVF_BLOCK_META, req, reply, sizeof(reply), &n);
    if (st != KVF_OK)
        return st;
    kvf_reader r = {.buffer = reply, .size = n};
    kvf_get_layer(&r, out);
    return kvf_reader_done(&r) && kvf_layer_equal(*out, req->layer) ? KVF_OK : KVF_PROTOCOL;
}

kvf_status kvf_client_stats(kvf_client c, kvf_directory_stats *s) {
    uint8_t bytes[52];
    size_t n;
    kvf_status st = call(c, KVF_STATS, NULL, bytes, sizeof(bytes), &n);
    if (st != KVF_OK)
        return st;
    kvf_reader r = {.buffer = bytes, .size = n};
    s->pages = kvf_get_u32(&r);
    s->workers = kvf_get_u32(&r);
    s->leases = kvf_get_u32(&r);
    s->publishes = kvf_get_u64(&r);
    s->duplicates = kvf_get_u64(&r);
    s->lookups = kvf_get_u64(&r);
    s->matched_pages = kvf_get_u64(&r);
    s->invalidations = kvf_get_u64(&r);
    return kvf_reader_done(&r) ? KVF_OK : KVF_PROTOCOL;
}
