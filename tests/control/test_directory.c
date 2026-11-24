/* Control-plane tests. Each fixture owns its tables and joins callers before teardown. */
#define _POSIX_C_SOURCE 200809L
#include "kvfabric/client.h"
#include "kvfabric/wire.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static kvf_worker_info make_worker(const char *name) {
    kvf_worker_info info = {.endpoint = {{127, 0, 0, 1}, 9101}, .capacity_pages = 32};
    snprintf(info.owner.name, sizeof(info.owner.name), "%s", name);
    assert(kvf_uuid_new(&info.owner.boot) == KVF_OK);
    return info;
}

static kvf_manifest make_page(kvf_owner owner, kvf_hash previous, int32_t token) {
    kvf_manifest m = {
        .owner = owner, .parent = previous, .token_count = 2, .layer_count = 2,
        .identity = {.tokens_per_page = 2, .layers = 2, .bytes_per_layer = 4096,
                     .dtype = KVF_F32, .tp_size = 1, .pp_size = 1},
        .layer = {{1, 1, 0, 4096, 17}, {2, 1, 0, 4096, 47}}
    };
    assert(kvf_hash_bytes("model", 5, &m.identity.model) == KVF_OK);
    assert(kvf_hash_bytes("tokenizer", 9, &m.identity.tokenizer) == KVF_OK);
    assert(kvf_hash_bytes("layout", 6, &m.identity.layout) == KVF_OK);
    assert(kvf_hash_bytes("tenant", 6, &m.identity.tenant) == KVF_OK);
    assert(kvf_identity_hash(&m.identity, &m.key.space) == KVF_OK);
    const int32_t tokens[] = {1, token};
    assert(kvf_page_hash(&m.identity, previous, tokens, 2, &m.key.page) == KVF_OK);
    return m;
}

static void test_directory(void) {
    kvf_directory *d;
    const kvf_directory_config config = {2, 1, 1, 1500, 200};
    assert(kvf_directory_create(config, &d) == KVF_OK);
    kvf_worker_info a = make_worker("a");
    kvf_worker_info reboot = make_worker("a");
    assert(kvf_directory_register(d, &a, (kvf_uuid){0}) == KVF_OK);
    kvf_worker_info b = make_worker("b");
    assert(kvf_directory_register(d, &b, (kvf_uuid){0}) == KVF_LIMIT);
    kvf_manifest m = make_page(a.owner, (kvf_hash){0}, 2);
    kvf_manifest n = make_page(a.owner, m.key.page, 3);
    kvf_manifest o = make_page(a.owner, n.key.page, 4);
    n.layer_count = 1;
    assert(kvf_directory_publish(d, &n) == KVF_INVALID);
    n.layer_count = 2;
    assert(kvf_directory_publish(d, &m) == KVF_OK);
    assert(kvf_directory_publish(d, &m) == KVF_OK);
    assert(kvf_directory_publish(d, &o) == KVF_OK);
    kvf_grant held;
    assert(kvf_directory_acquire(d, o.key, &held) == KVF_OK);
    assert(kvf_directory_publish(d, &n) == KVF_LIMIT);
    assert(kvf_directory_release(d, held.ticket) == KVF_OK);
    kvf_hash pages[] = {m.key.page, n.key.page, o.key.page};
    uint32_t count;
    assert(kvf_directory_lookup_prefix(d, m.key.space, pages, 3, &count) == KVF_OK && count == 1);
    kvf_hash other = m.key.space;
    ++other.bytes[0];
    assert(kvf_directory_lookup_prefix(d, other, pages, 3, &count) == KVF_OK && count == 0);
    assert(kvf_directory_invalidate(d, &o) == KVF_OK);
    assert(kvf_directory_publish(d, &n) == KVF_OK);
    assert(kvf_directory_lookup_prefix(d, m.key.space, pages, 3, &count) == KVF_OK && count == 2);

    kvf_grant g;
    assert(kvf_directory_acquire(d, m.key, &g) == KVF_OK);
    kvf_fetch_request r = {.key = m.key, .owner_boot = a.owner.boot, .ticket = g.ticket, .layer = m.layer[0]};
    assert(kvf_directory_acquire(d, n.key, &g) == KVF_LIMIT);
    assert(kvf_directory_validate(d, &r) == KVF_OK);
    ++r.layer.generation;
    assert(kvf_directory_validate(d, &r) == KVF_STALE);
    --r.layer.generation;
    ++r.layer.version;
    assert(kvf_directory_validate(d, &r) == KVF_STALE);
    --r.layer.version;
    ++r.ticket.secret.bytes[0];
    assert(kvf_directory_validate(d, &r) == KVF_STALE);
    --r.ticket.secret.bytes[0];
    r.layer_index = KVF_MAX_LAYERS;
    assert(kvf_directory_validate(d, &r) == KVF_INVALID);
    r.layer_index = 0;
    uint32_t ttl;
    assert(kvf_directory_renew(d, r.ticket, &ttl) == KVF_OK && ttl == config.lease_ttl_ms);
    assert(nanosleep(&(struct timespec){.tv_nsec = 230000000}, NULL) == 0);
    assert(kvf_directory_validate(d, &r) == KVF_STALE);
    assert(kvf_directory_renew(d, r.ticket, &ttl) == KVF_STALE);
    assert(kvf_directory_release(d, r.ticket) == KVF_STALE);
    assert(kvf_directory_acquire(d, m.key, &g) == KVF_OK && g.ticket.fence > r.ticket.fence);
    assert(kvf_directory_release(d, r.ticket) == KVF_STALE);
    r.ticket = g.ticket;
    assert(kvf_directory_release(d, g.ticket) == KVF_OK);
    assert(kvf_directory_validate(d, &r) == KVF_STALE);

    assert(kvf_directory_acquire(d, n.key, &g) == KVF_OK);
    assert(kvf_directory_invalidate(d, &n) == KVF_OK);
    kvf_manifest changed = n;
    ++changed.layer[0].version;
    assert(kvf_directory_publish(d, &changed) == KVF_OK);
    r = (kvf_fetch_request){.key = n.key, .owner_boot = a.owner.boot, .ticket = g.ticket, .layer = n.layer[0]};
    assert(kvf_directory_validate(d, &r) == KVF_STALE);
    assert(kvf_directory_invalidate(d, &n) == KVF_STALE);
    assert(kvf_directory_register(d, &reboot, (kvf_uuid){0}) == KVF_STALE);
    assert(kvf_directory_register(d, &reboot, a.owner.boot) == KVF_OK);
    assert(kvf_directory_heartbeat(d, &a) == KVF_STALE);
    assert(kvf_directory_register(d, &a, (kvf_uuid){0}) == KVF_STALE);
    assert(kvf_directory_publish(d, &m) == KVF_STALE);
    assert(kvf_directory_acquire(d, m.key, &g) == KVF_NOT_FOUND);
    m.owner = reboot.owner;
    assert(kvf_directory_publish(d, &m) == KVF_OK);
    kvf_directory_stats s;
    kvf_directory_get_stats(d, &s);
    assert(s.workers == 1 && s.pages == 1 && s.duplicates == 1 && s.leases == 0 && s.invalidations == 2);
    assert(kvf_directory_unregister(d, &reboot.owner) == KVF_OK);
    assert(kvf_directory_acquire(d, m.key, &g) == KVF_NOT_FOUND);
    kvf_directory_destroy(d);
}

static void test_replicas_and_eviction(void) {
    kvf_directory *d;
    assert(kvf_directory_create((kvf_directory_config){4, 2, 8, 3000, 2000}, &d) == KVF_OK);
    kvf_worker_info a = make_worker("longer"), b = make_worker("lower-load");
    a.active_transfers = 5;
    assert(kvf_directory_register(d, &a, (kvf_uuid){0}) == KVF_OK);
    assert(kvf_directory_register(d, &b, (kvf_uuid){0}) == KVF_OK);
    kvf_manifest root = make_page(a.owner, (kvf_hash){0}, 2);
    kvf_manifest leaf = make_page(a.owner, root.key.page, 3);
    kvf_manifest root_copy = root, leaf_copy = leaf;
    root_copy.owner = leaf_copy.owner = b.owner;
    assert(kvf_directory_publish(d, &root) == KVF_OK);
    assert(kvf_directory_publish(d, &leaf) == KVF_OK);
    assert(kvf_directory_publish(d, &root_copy) == KVF_OK);
    ++leaf_copy.layer[0].checksum;
    assert(kvf_directory_publish(d, &leaf_copy) == KVF_CONFLICT);
    --leaf_copy.layer[0].checksum;
    kvf_hash hashes[] = {root.key.page, leaf.key.page};
    kvf_grant grant;
    assert(kvf_directory_acquire_prefix(d, root.key.space, hashes, 2, &grant) == KVF_OK);
    assert(kvf_owner_equal(&grant.manifest.owner, &a.owner));
    assert(kvf_directory_release(d, grant.ticket) == KVF_OK);
    assert(kvf_directory_publish(d, &leaf_copy) == KVF_OK);
    assert(kvf_directory_acquire_prefix(d, root.key.space, hashes, 2, &grant) == KVF_OK);
    assert(kvf_owner_equal(&grant.manifest.owner, &b.owner));
    assert(kvf_directory_release(d, grant.ticket) == KVF_OK);
    assert(kvf_directory_acquire(d, leaf.key, &grant) == KVF_OK);
    assert(kvf_owner_equal(&grant.manifest.owner, &b.owner));
    assert(kvf_directory_retire(d, &leaf_copy) == KVF_CONFLICT);
    assert(kvf_directory_retire(d, &root) == KVF_CONFLICT);
    kvf_manifest incoming = make_page(a.owner, root.key.page, 9);
    assert(kvf_directory_publish(d, &incoming) == KVF_OK);
    kvf_manifest looked_up;
    kvf_endpoint endpoint;
    assert(kvf_directory_lookup_page(d, root.key, &looked_up, &endpoint) == KVF_OK);
    assert(kvf_directory_invalidate(d, &leaf) == KVF_NOT_FOUND);
    kvf_fetch_request request = {.key = leaf.key, .owner_boot = b.owner.boot,
                                 .ticket = grant.ticket, .layer = leaf_copy.layer[0]};
    assert(kvf_directory_validate(d, &request) == KVF_OK);
    assert(kvf_directory_release(d, grant.ticket) == KVF_OK);
    assert(kvf_directory_retire(d, &leaf_copy) == KVF_OK);
    kvf_directory_destroy(d);
}

static void test_worker_expiry_and_new_epoch(void) {
    kvf_directory *d;
    const kvf_directory_config config = {1, 1, 1, 200, 2000};
    assert(kvf_directory_create(config, &d) == KVF_OK);
    kvf_worker_info a = make_worker("expiring");
    kvf_worker_info b = make_worker("replacement");
    kvf_manifest m = make_page(a.owner, (kvf_hash){0}, 2);
    assert(kvf_directory_register(d, &a, (kvf_uuid){0}) == KVF_OK);
    assert(kvf_directory_publish(d, &m) == KVF_OK);
    kvf_grant old;
    assert(kvf_directory_acquire(d, m.key, &old) == KVF_OK);
    assert(nanosleep(&(struct timespec){.tv_nsec = 230000000}, NULL) == 0);
    assert(kvf_directory_register(d, &b, (kvf_uuid){0}) == KVF_OK);
    assert(kvf_directory_heartbeat(d, &a) == KVF_NOT_FOUND);
    m.owner = b.owner;
    assert(kvf_directory_publish(d, &m) == KVF_OK);
    kvf_grant latest;
    assert(kvf_directory_acquire(d, m.key, &latest) == KVF_OK);
    assert(latest.ticket.slot == old.ticket.slot && latest.ticket.fence > old.ticket.fence);
    assert(kvf_directory_release(d, old.ticket) == KVF_STALE);
    kvf_directory_destroy(d);

    assert(kvf_directory_create(config, &d) == KVF_OK);
    assert(kvf_directory_register(d, &b, (kvf_uuid){0}) == KVF_OK);
    assert(kvf_directory_publish(d, &m) == KVF_OK);
    kvf_grant fresh;
    assert(kvf_directory_acquire(d, m.key, &fresh) == KVF_OK);
    assert(!kvf_uuid_equal(latest.ticket.directory, fresh.ticket.directory));
    kvf_fetch_request r = {
        .key = m.key, .owner_boot = b.owner.boot, .layer = m.layer[0], .ticket = fresh.ticket
    };
    assert(kvf_directory_validate(d, &r) == KVF_OK);
    r.ticket.directory = latest.ticket.directory;
    assert(kvf_directory_validate(d, &r) == KVF_STALE);
    kvf_directory_destroy(d);
}

static void test_batch_fences(void) {
    kvf_directory *d;
    const kvf_directory_config config = {8, 2, 8, 5000, 5000};
    assert(kvf_directory_create(config, &d) == KVF_OK);
    kvf_worker_info a = make_worker("batch-a"), b = make_worker("batch-b");
    assert(kvf_directory_register(d, &a, (kvf_uuid){0}) == KVF_OK);
    assert(kvf_directory_register(d, &b, (kvf_uuid){0}) == KVF_OK);
    kvf_manifest root = make_page(a.owner, (kvf_hash){0}, 2);
    kvf_manifest leaf = make_page(b.owner, root.key.page, 3);
    assert(kvf_directory_publish(d, &root) == KVF_OK);
    assert(kvf_directory_publish(d, &leaf) == KVF_OK);
    kvf_hash keys[] = {root.key.page, leaf.key.page, {{1}}};
    kvf_grant grants[3];
    uint32_t count;
    assert(kvf_directory_acquire_batch(d, root.key.space, keys, 3, grants, &count) == KVF_OK && count == 2);
    assert(kvf_owner_equal(&grants[0].manifest.owner, &a.owner));
    assert(kvf_owner_equal(&grants[1].manifest.owner, &b.owner));
    kvf_batch_page pages[2];
    for (unsigned i = 0; i < 2; ++i) {
        const kvf_manifest *m = &grants[i].manifest;
        pages[i] = (kvf_batch_page){.key = m->key, .ticket = grants[i].ticket, .owner_boot = m->owner.boot};
        assert(kvf_manifest_digest(m, &pages[i].manifest_digest) == KVF_OK);
    }
    assert(kvf_directory_check_batch(d, pages, 2, 0) == KVF_OK);
    kvf_batch_page wrong = pages[0];
    ++wrong.owner_boot.bytes[0];
    assert(kvf_directory_check_batch(d, &wrong, 1, 0) == KVF_STALE);
    wrong = pages[0];
    ++wrong.ticket.fence;
    assert(kvf_directory_check_batch(d, &wrong, 1, 0) == KVF_STALE);
    wrong = pages[1];
    ++wrong.ticket.directory.bytes[0];
    assert(kvf_directory_check_batch(d, &wrong, 1, 0) == KVF_STALE);
    kvf_manifest changed = leaf;
    ++changed.layer[1].generation;
    changed.layer[1].id += UINT64_C(1) << 32;
    ++changed.layer[1].version;
    wrong = pages[1];
    assert(kvf_manifest_digest(&changed, &wrong.manifest_digest) == KVF_OK);
    assert(kvf_directory_check_batch(d, &wrong, 1, 0) == KVF_STALE);
    assert(kvf_directory_invalidate(d, &leaf) == KVF_OK);
    assert(kvf_directory_publish(d, &changed) == KVF_OK);
    assert(kvf_directory_check_batch(d, pages, 2, 1) == KVF_STALE);
    assert(kvf_directory_check_batch(d, pages, 1, 0) == KVF_STALE);
    assert(kvf_directory_acquire_batch(d, root.key.space, keys, 2, grants, &count) == KVF_OK && count == 2);
    assert(grants[1].ticket.fence > pages[1].ticket.fence);
    assert(kvf_directory_check_batch(d, pages, 2, 0) == KVF_STALE);
    kvf_directory_destroy(d);
}

typedef struct {
    kvf_client client;
    kvf_worker_info info;
    unsigned index;
} peer;

static void *publish_and_lookup(void *arg) {
    peer *p = arg;
    kvf_manifest m = make_page(p->info.owner, (kvf_hash){0}, (int32_t)p->index + 2);
    for (unsigned i = 0; i < 20; ++i) {
        assert(kvf_client_publish(p->client, &m) == KVF_OK);
        uint32_t n;
        assert(kvf_client_lookup_prefix(p->client, m.key.space, &m.key.page, 1, &n) == KVF_OK && n == 1);
        kvf_grant g;
        assert(kvf_client_acquire(p->client, m.key, &g) == KVF_OK);
        kvf_fetch_request r = {
            .key = m.key, .owner_boot = m.owner.boot, .layer = m.layer[0], .ticket = g.ticket
        };
        assert(kvf_client_validate(p->client, &r) == KVF_OK);
        assert(kvf_client_release(p->client, r.ticket) == KVF_OK);
    }
    return NULL;
}

static void test_rpc(void) {
    kvf_directory *d;
    kvf_directory_config cfg;
    kvf_directory_defaults(&cfg);
    assert(kvf_directory_create(cfg, &d) == KVF_OK);
    kvf_server *server;
    assert(kvf_control_start(d, (kvf_endpoint){{127, 0, 0, 1}, 0}, 8, 2000, &server) == KVF_OK);
    kvf_client c = {kvf_server_endpoint(server), 2000};
    kvf_worker_info a = make_worker("wire");
    assert(kvf_client_register(c, &a, (kvf_uuid){0}) == KVF_OK);
    assert(kvf_client_heartbeat(c, &a) == KVF_OK);
    kvf_worker_info status;
    int online;
    assert(kvf_client_worker_status(c, a.owner.name, &status, &online) == KVF_OK && online);
    assert(kvf_owner_equal(&status.owner, &a.owner));
    enum { CLIENTS = 8, ROUNDS = 20 };
    pthread_t threads[CLIENTS];
    peer peers[CLIENTS];
    for (unsigned i = 0; i < CLIENTS; ++i) {
        peers[i] = (peer){c, a, i};
        assert(pthread_create(&threads[i], NULL, publish_and_lookup, &peers[i]) == 0);
    }
    for (unsigned i = 0; i < CLIENTS; ++i)
        assert(pthread_join(threads[i], NULL) == 0);

    uint64_t deadline = kvf_now_ms() + 1000;
    int fd;
    assert(kvf_connect(c.endpoint, deadline, &fd) == KVF_OK);
    kvf_frame req = {.opcode = KVF_STATS, .request_id = 7};
    assert(kvf_send_frame(fd, req, NULL, deadline) == KVF_OK);
    uint8_t reply[52];
    kvf_frame response;
    assert(kvf_recv_frame(fd, &response, reply, sizeof(reply), deadline) == KVF_OK);
    assert(response.response && response.request_id == req.request_id);
    /* The first response consumes the entire connection's request allowance. */
    (void)kvf_send_frame(fd, req, NULL, deadline);
    assert(kvf_recv_frame(fd, &response, reply, sizeof(reply), deadline) == KVF_IO);
    assert(close(fd) == 0);

    kvf_directory_stats stats;
    assert(kvf_client_stats(c, &stats) == KVF_OK);
    assert(stats.pages == CLIENTS && stats.publishes == CLIENTS && stats.leases == 0);
    assert(stats.duplicates == CLIENTS * (ROUNDS - 1) && stats.matched_pages == CLIENTS * ROUNDS);
    kvf_server_stop(server);
    kvf_directory_destroy(d);
}

int main(void) {
    test_directory();
    test_replicas_and_eviction();
    test_worker_expiry_and_new_epoch();
    test_batch_fences();
    test_rpc();
    puts("KV-Fabric directory and control RPC: PASS");
    return 0;
}
