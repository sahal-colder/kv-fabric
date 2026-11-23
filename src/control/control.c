/* Control RPC dispatch. Stack frames own message bytes. Directory unlocks before TCP writes. */
#include "kvfabric/directory.h"
#include "kvfabric/wire.h"
#include <string.h>

static void put_grant(kvf_writer *w, const kvf_grant *g) {
    kvf_put_manifest(w, &g->manifest);
    kvf_put_endpoint(w, g->endpoint);
    kvf_put_ticket(w, g->ticket);
    kvf_put_u32(w, g->ttl_ms);
}

static kvf_status dispatch(kvf_directory *d, kvf_opcode op, kvf_reader *r, kvf_writer *w) {
    switch (op) {
    case KVF_REGISTER:
    case KVF_HEARTBEAT: {
        kvf_worker_info info;
        kvf_uuid replaces = {0};
        kvf_get_worker(r, &info);
        if (op == KVF_REGISTER)
            kvf_get_bytes(r, replaces.bytes, 16);
        if (!kvf_reader_done(r))
            return KVF_INVALID;
        return op == KVF_REGISTER ? kvf_directory_register(d, &info, replaces) :
            kvf_directory_heartbeat(d, &info);
    }
    case KVF_UNREGISTER: {
        kvf_owner owner;
        kvf_get_owner(r, &owner);
        return kvf_reader_done(r) ? kvf_directory_unregister(d, &owner) : KVF_INVALID;
    }
    case KVF_WORKER_STATUS: {
        char name[KVF_WORKER_NAME] = {0};
        uint16_t n = kvf_get_u16(r);
        if (!n || n >= sizeof(name))
            return KVF_INVALID;
        kvf_get_bytes(r, name, n);
        if (!kvf_reader_done(r) || memchr(name, '\0', n))
            return KVF_INVALID;
        kvf_worker_info info;
        int online;
        kvf_status st = kvf_directory_worker_status(d, name, &info, &online);
        if (st == KVF_OK) {
            kvf_put_u16(w, (uint16_t)online);
            kvf_put_worker(w, &info);
        }
        return st;
    }
    case KVF_PUBLISH:
    case KVF_INVALIDATE:
    case KVF_RETIRE: {
        kvf_manifest manifest;
        kvf_get_manifest(r, &manifest);
        if (!kvf_reader_done(r))
            return KVF_INVALID;
        if (op == KVF_PUBLISH)
            return kvf_directory_publish(d, &manifest);
        return op == KVF_RETIRE ? kvf_directory_retire(d, &manifest) :
            kvf_directory_invalidate(d, &manifest);
    }
    case KVF_LOOKUP_PREFIX:
    case KVF_ACQUIRE_PREFIX: {
        kvf_hash space, pages[KVF_MAX_LOOKUP];
        kvf_get_bytes(r, space.bytes, 32);
        uint32_t count = kvf_get_u32(r);
        if (!count || count > KVF_MAX_LOOKUP)
            return KVF_INVALID;
        for (uint32_t i = 0; i < count; ++i)
            kvf_get_bytes(r, pages[i].bytes, 32);
        if (!kvf_reader_done(r))
            return KVF_INVALID;
        if (op == KVF_ACQUIRE_PREFIX) {
            kvf_grant g;
            kvf_status st = kvf_directory_acquire_prefix(d, space, pages, count, &g);
            if (st == KVF_OK)
                put_grant(w, &g);
            return st;
        }
        uint32_t matched;
        kvf_status st = kvf_directory_lookup_prefix(d, space, pages, count, &matched);
        if (st == KVF_OK)
            kvf_put_u32(w, matched);
        return st;
    }
    case KVF_LOOKUP_PAGE:
    case KVF_ACQUIRE: {
        kvf_page_key key;
        kvf_get_key(r, &key);
        if (!kvf_reader_done(r))
            return KVF_INVALID;
        kvf_grant g;
        kvf_status st = op == KVF_ACQUIRE ? kvf_directory_acquire(d, key, &g) :
            kvf_directory_lookup_page(d, key, &g.manifest, &g.endpoint);
        if (st == KVF_OK && op == KVF_ACQUIRE)
            put_grant(w, &g);
        if (st == KVF_OK && op == KVF_LOOKUP_PAGE) {
            kvf_put_manifest(w, &g.manifest);
            kvf_put_endpoint(w, g.endpoint);
        }
        return st;
    }
    case KVF_RENEW:
    case KVF_RELEASE: {
        kvf_ticket ticket;
        kvf_get_ticket(r, &ticket);
        if (!kvf_reader_done(r))
            return KVF_INVALID;
        if (op == KVF_RELEASE)
            return kvf_directory_release(d, ticket);
        uint32_t ttl;
        kvf_status st = kvf_directory_renew(d, ticket, &ttl);
        if (st == KVF_OK)
            kvf_put_u32(w, ttl);
        return st;
    }
    case KVF_VALIDATE: {
        kvf_fetch_request request;
        kvf_get_fetch(r, &request);
        return kvf_reader_done(r) ? kvf_directory_validate(d, &request) : KVF_INVALID;
    }
    case KVF_STATS: {
        if (!kvf_reader_done(r))
            return KVF_INVALID;
        kvf_directory_stats s;
        kvf_directory_get_stats(d, &s);
        kvf_put_u32(w, s.pages);
        kvf_put_u32(w, s.workers);
        kvf_put_u32(w, s.leases);
        kvf_put_u64(w, s.publishes);
        kvf_put_u64(w, s.duplicates);
        kvf_put_u64(w, s.lookups);
        kvf_put_u64(w, s.matched_pages);
        kvf_put_u64(w, s.invalidations);
        return KVF_OK;
    }
    case KVF_FETCH:
    case KVF_FETCH_STREAM:
    case KVF_BLOCK_META:
        return KVF_PROTOCOL;
    }
    return KVF_PROTOCOL;
}

static void control_request(void *context, unsigned lane, int fd, uint64_t deadline) {
    (void)lane;
    uint8_t bytes[KVF_METADATA_BYTES], reply[KVF_METADATA_BYTES];
    kvf_frame f;
    if (kvf_recv_frame(fd, &f, bytes, sizeof(bytes), deadline) != KVF_OK || f.response)
        return;
    kvf_reader r = {.buffer = bytes, .size = f.size};
    kvf_writer w = {.buffer = reply, .capacity = sizeof(reply)};
    f.status = dispatch(context, f.opcode, &r, &w);
    if (w.failed)
        f.status = KVF_INTERNAL;
    f.response = 1;
    f.size = f.status == KVF_OK ? (uint32_t)w.size : 0;
    /* A broken peer may leave an acquired lease. Its expiry bounds that state. */
    (void)kvf_send_frame(fd, f, reply, deadline);
}

kvf_status kvf_control_start(kvf_directory *d, kvf_endpoint listen, unsigned lanes,
                              uint32_t timeout_ms, kvf_server **out) {
    if (!d)
        return KVF_INVALID;
    return kvf_server_start(listen, lanes, timeout_ms, control_request, d, out);
}
