/* Data-plane fault tests with real TCP relays. Fixtures own Workers and Storage. No test lock nests. */
#define _GNU_SOURCE
#include "../../src/worker/remote/internal.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { BYTES = 4096, LAYERS = 2, PAGES = 2, TPP = 2 };
typedef enum { DISCONNECT, BAD_CRC, EXPIRE, EVICT, PAUSE } fault;
static const int32_t tokens[] = {101, 102, 103, 104};

typedef struct {
    mi_mempool *pool;
    mi_storage_backend *cpu;
    mi_tiered_storage *storage;
    mi_remote_worker *remote;
} fixture;

static fixture make_worker(kvf_identity identity, const char *name, kvf_endpoint control,
                           kvf_endpoint advertise) {
    fixture f = {0};
    assert(mi_mempool_create(MI_MEMORY_CPU, -1, (mi_mempool_config){0}, &f.pool) == MI_OK);
    assert(mi_memory_backend_create(MI_LOCATION_CPU, BYTES, f.pool, LAYERS * PAGES, &f.cpu) == MI_OK);
    mi_storage_config storage;
    mi_storage_config_default(&storage);
    storage.block_bytes = BYTES;
    storage.cpu_bytes = BYTES * LAYERS * PAGES;
    assert(mi_tiered_storage_create(storage, (mi_storage_backends){.cpu = f.cpu}, &f.storage) == MI_OK);
    mi_remote_config cfg;
    mi_remote_config_defaults(&cfg);
    cfg.identity = identity;
    cfg.control = control;
    cfg.advertise = advertise;
    cfg.heartbeat_ms = 100;
    cfg.lanes = 2;
    snprintf(cfg.worker_name, sizeof(cfg.worker_name), "%s", name);
    assert(mi_remote_worker_start(f.storage, cfg, &f.remote) == KVF_OK);
    return f;
}

static void close_worker(fixture f) {
    mi_remote_worker_stop(f.remote);
    mi_tiered_storage_destroy(f.storage);
    mi_backend_destroy(f.cpu);
    mi_mempool_destroy(f.pool);
}

static void write_page(mi_kv_sequence *sequence, size_t page) {
    assert(mi_kv_sequence_append_page(sequence, MI_LOCATION_CPU) == MI_OK);
    for (size_t layer = 0; layer < LAYERS; ++layer) {
        mi_page_lease lease;
        assert(mi_paged_kv_acquire(sequence, page, layer, MI_ACCESS_WRITE, NULL, &lease) == MI_OK);
        memset(lease.lease.data, (int)(1 + page * LAYERS + layer), BYTES);
        assert(mi_page_lease_finish(&lease) == MI_OK);
    }
}

static void publish(fixture source) {
    mi_kv_sequence *s;
    assert(mi_kv_sequence_create(mi_remote_worker_cache(source.remote), &s) == MI_OK);
    for (size_t page = 0; page < PAGES; ++page)
        write_page(s, page);
    assert(mi_remote_publish(source.remote, s, tokens, PAGES * TPP, PAGES) == KVF_OK);
    mi_kv_sequence_destroy(s);
}

typedef struct {
    atomic_int armed;
    atomic_int triggered;
    atomic_int proceed;
    atomic_uint connections;
    fault mode;
    fixture source;
    kvf_endpoint upstream;
    kvf_page_key failure_page;
} proxy;

static void send_short(int fd, kvf_frame frame, const uint8_t *data) {
    uint8_t wire[KVF_HEADER_BYTES + 32];
    kvf_writer w = {.buffer = wire, .capacity = sizeof(wire)};
    kvf_put_u32(&w, UINT32_C(0x4b564631));
    kvf_put_u16(&w, KVF_WIRE_VERSION);
    kvf_put_u16(&w, KVF_HEADER_BYTES);
    kvf_put_u16(&w, (uint16_t)frame.opcode);
    kvf_put_u16(&w, 1);
    kvf_put_u32(&w, KVF_OK);
    kvf_put_u64(&w, frame.request_id);
    kvf_put_u32(&w, BYTES);
    kvf_put_u32(&w, kvf_crc32c(data, BYTES));
    kvf_put_u32(&w, kvf_crc32c(wire, w.size));
    kvf_put_bytes(&w, data, 32);
    assert(!w.failed && w.size == sizeof(wire));
    assert(send(fd, wire, sizeof(wire), MSG_NOSIGNAL) == (ssize_t)sizeof(wire));
}

static int forward_layer(proxy *p, int fd, uint64_t deadline) {
    uint8_t wire[256], bytes[BYTES];
    kvf_frame frame;
    if (kvf_recv_frame(fd, &frame, wire, sizeof(wire), deadline) != KVF_OK)
        return 0;
    assert(frame.opcode == KVF_FETCH_STREAM && !frame.response);
    kvf_reader reader = {.buffer = wire, .size = frame.size};
    kvf_fetch_request r;
    kvf_get_fetch(&reader, &r);
    assert(kvf_reader_done(&reader));
    assert(kvf_client_fetch(p->upstream, 1500, &r, bytes, sizeof(bytes)) == KVF_OK);

    frame.response = 1;
    frame.size = sizeof(bytes);
    if (kvf_page_key_equal(r.key, p->failure_page) && r.layer_index == LAYERS - 1) {
        atomic_store_explicit(&p->triggered, 1, memory_order_release);
        switch (p->mode) {
        case DISCONNECT:
            send_short(fd, frame, bytes);
            return 0;
        case BAD_CRC:
            bytes[17] ^= 1;
            break;
        case EXPIRE:
            assert(nanosleep(&(struct timespec){.tv_nsec = 650000000}, NULL) == 0);
            break;
        case EVICT: {
            assert(mi_remote_evict_prefixes(p->source.remote) == KVF_OK);
            mi_block_meta meta;
            assert(mi_storage_metadata(p->source.storage, r.layer.id, &meta) == MI_ENOENT);
            publish(p->source);
            uint8_t rejected[BYTES];
            assert(kvf_client_fetch(p->upstream, 1500, &r, rejected, sizeof(rejected)) == KVF_STALE);
            break;
        }
        case PAUSE:
            while (!atomic_load_explicit(&p->proceed, memory_order_acquire) && kvf_now_ms() < deadline)
                nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
            assert(atomic_load_explicit(&p->proceed, memory_order_acquire));
            break;
        }
    }
    assert(kvf_send_frame(fd, frame, bytes, deadline) == KVF_OK);
    return 1;
}

static void forward(void *context, unsigned lane, int fd, uint64_t deadline) {
    (void)lane;
    proxy *p = context;
    assert(atomic_load_explicit(&p->armed, memory_order_acquire));
    atomic_fetch_add_explicit(&p->connections, 1, memory_order_relaxed);
    while (forward_layer(p, fd, deadline))
        deadline = kvf_now_ms() + 2000;
}

static void exercise(fault mode, const char *name, kvf_status expected) {
    kvf_directory_config cfg;
    kvf_directory_defaults(&cfg);
    cfg.lease_ttl_ms = mode == PAUSE ? 3000 : 450;
    kvf_directory *d;
    kvf_server *control, *relay;
    kvf_endpoint loop = {{127, 0, 0, 1}, 0};
    assert(kvf_directory_create(cfg, &d) == KVF_OK);
    assert(kvf_control_start(d, loop, 4, 2000, &control) == KVF_OK);
    kvf_endpoint endpoint = kvf_server_endpoint(control);

    kvf_identity id = {
        .tokens_per_page = TPP, .layers = LAYERS, .bytes_per_layer = BYTES,
        .dtype = KVF_F32, .tp_size = 1, .pp_size = 1
    };
    assert(kvf_hash_bytes("model", 5, &id.model) == KVF_OK);
    assert(kvf_hash_bytes("tokenizer", 9, &id.tokenizer) == KVF_OK);
    assert(kvf_hash_bytes("layout", 6, &id.layout) == KVF_OK);
    assert(kvf_hash_bytes("tenant", 6, &id.tenant) == KVF_OK);
    proxy p = {.mode = mode};
    atomic_init(&p.armed, 0);
    atomic_init(&p.triggered, 0);
    atomic_init(&p.proceed, 0);
    atomic_init(&p.connections, 0);
    kvf_hash first;
    assert(kvf_identity_hash(&id, &p.failure_page.space) == KVF_OK);
    assert(kvf_page_hash(&id, (kvf_hash){0}, tokens, TPP, &first) == KVF_OK);
    assert(kvf_page_hash(&id, first, tokens + TPP, TPP, &p.failure_page.page) == KVF_OK);
    assert(kvf_server_start(loop, 1, 2500, forward, &p, &relay) == KVF_OK);
    p.source = make_worker(id, "source", endpoint, kvf_server_endpoint(relay));
    fixture target = make_worker(id, "target", endpoint, (kvf_endpoint){0});
    /* The advertised endpoint is the relay. Use the bound data endpoint reported by the API. */
    p.upstream = mi_remote_worker_listen_endpoint(p.source.remote);
    atomic_store_explicit(&p.armed, 1, memory_order_release);
    publish(p.source);
    kvf_manifest former;
    kvf_endpoint published_address;
    kvf_client client = {endpoint, 1500};
    assert(kvf_client_lookup_page(client, p.failure_page, &former, &published_address) == KVF_OK);

    mi_kv_sequence *reused;
    mi_remote_result result;
    if (mode == PAUSE) {
        mi_remote_prefetch *running, *queued[8], *overflow;
        assert(mi_remote_prefetch_async(target.remote, tokens, PAGES * TPP, &running) == KVF_OK);
        uint64_t deadline = kvf_now_ms() + 1700;
        while (!atomic_load_explicit(&p.triggered, memory_order_acquire) && kvf_now_ms() < deadline)
            nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
        assert(atomic_load_explicit(&p.triggered, memory_order_acquire));
        for (unsigned i = 0; i < 8; ++i) {
            assert(mi_remote_prefetch_async(target.remote, tokens, PAGES * TPP, &queued[i]) == KVF_OK);
            mi_remote_prefetch_cancel(queued[i]);
        }
        assert(mi_remote_prefetch_async(target.remote, tokens, PAGES * TPP, &overflow) == KVF_LIMIT);
        mi_remote_prefetch_cancel(running);
        atomic_store_explicit(&p.proceed, 1, memory_order_release);
        assert(mi_remote_prefetch_wait(running, &reused, &result) == KVF_OK);
        mi_remote_prefetch_destroy(running);
        for (unsigned i = 0; i < 8; ++i) {
            mi_kv_sequence *none;
            mi_remote_result cancelled;
            assert(mi_remote_prefetch_wait(queued[i], &none, &cancelled) == KVF_CANCELLED && !none);
            mi_remote_prefetch_destroy(queued[i]);
        }
    } else {
        assert(mi_remote_resolve(target.remote, tokens, PAGES * TPP, &reused, &result) == KVF_OK);
    }
    assert(atomic_load_explicit(&p.triggered, memory_order_acquire));
    assert(atomic_load_explicit(&p.connections, memory_order_relaxed) == 1);
    if (result.remote_status != expected)
        fprintf(stderr, "%s returned %s, wanted %s\n",
                name, kvf_status_string(result.remote_status), kvf_status_string(expected));
    assert(result.remote_status == expected && result.remote_tokens == TPP && result.local_tokens == 0);
    assert(mi_kv_sequence_page_count(reused) == 1);
    mi_kv_sequence *match;
    size_t matched;
    mi_paged_kv *cache = mi_remote_worker_cache(target.remote);
    assert(mi_paged_kv_match(cache, tokens, PAGES * TPP, &match, &matched) == MI_OK && matched == TPP);
    mi_kv_sequence_destroy(match);
    mi_page_lease lease;
    assert(mi_paged_kv_acquire(reused, 0, 0, MI_ACCESS_READ, NULL, &lease) == MI_OK);
    assert(((const uint8_t *)lease.lease.data)[BYTES - 1] == 1);
    assert(mi_page_lease_finish(&lease) == MI_OK);

    /* Model recompute can use the released slots of the failed page. */
    write_page(reused, 1);
    assert(mi_kv_sequence_publish(reused, tokens, PAGES * TPP, PAGES) == MI_OK);
    mi_kv_sequence_destroy(reused);

    if (mode == EVICT) {
        kvf_manifest current;
        assert(kvf_client_lookup_page(client, p.failure_page, &current, &published_address) == KVF_OK);
        for (size_t i = 0; i < LAYERS; ++i)
            assert(current.layer[i].generation > former.layer[i].generation);
    }
    close_worker(target);
    kvf_server_stop(relay);
    close_worker(p.source);
    kvf_server_stop(control);
    kvf_directory_destroy(d);
    printf("%s: safe prefix retained and failed suffix rolled back\n", name);
}

static void leaf_pressure(void) {
    kvf_directory_config cfg;
    kvf_directory_defaults(&cfg);
    kvf_directory *d;
    kvf_server *control;
    assert(kvf_directory_create(cfg, &d) == KVF_OK);
    assert(kvf_control_start(d, (kvf_endpoint){{127, 0, 0, 1}, 0}, 4, 2000, &control) == KVF_OK);
    kvf_client client = {kvf_server_endpoint(control), 1500};
    kvf_identity id = {.tokens_per_page = TPP, .layers = LAYERS, .bytes_per_layer = BYTES,
                      .dtype = KVF_F32, .tp_size = 1, .pp_size = 1};
    assert(kvf_hash_bytes("model", 5, &id.model) == KVF_OK);
    assert(kvf_hash_bytes("tokenizer", 9, &id.tokenizer) == KVF_OK);
    assert(kvf_hash_bytes("layout", 6, &id.layout) == KVF_OK);
    assert(kvf_hash_bytes("tenant", 6, &id.tenant) == KVF_OK);
    fixture source = make_worker(id, "leaf-test", client.endpoint, (kvf_endpoint){0});
    publish(source);
    kvf_page_key leaf;
    kvf_hash root;
    assert(kvf_identity_hash(&id, &leaf.space) == KVF_OK);
    assert(kvf_page_hash(&id, (kvf_hash){0}, tokens, TPP, &root) == KVF_OK);
    assert(kvf_page_hash(&id, root, tokens + TPP, TPP, &leaf.page) == KVF_OK);
    kvf_grant grant;
    assert(kvf_client_acquire(client, leaf, &grant) == KVF_OK);
    mi_kv_sequence *new_page, *held;
    mi_paged_kv *cache = mi_remote_worker_cache(source.remote);
    assert(mi_kv_sequence_create(cache, &new_page) == MI_OK);
    assert(mi_remote_append_page(source.remote, new_page) == KVF_LIMIT);
    assert(kvf_client_release(client, grant.ticket) == KVF_OK);
    size_t matched;
    assert(mi_paged_kv_match(cache, tokens, PAGES * TPP, &held, &matched) == MI_OK && matched == PAGES * TPP);
    assert(mi_remote_append_page(source.remote, new_page) == KVF_LIMIT);
    mi_kv_sequence_destroy(held);
    assert(mi_remote_append_page(source.remote, new_page) == KVF_OK);
    assert(kvf_client_acquire(client, leaf, &grant) == KVF_NOT_FOUND);
    assert(mi_paged_kv_match(cache, tokens, PAGES * TPP, &held, &matched) == MI_OK && matched == TPP);
    mi_kv_sequence_destroy(held);
    mi_kv_sequence_destroy(new_page);
    close_worker(source);
    kvf_server_stop(control);
    kvf_directory_destroy(d);
}

static void probe_peer(void *context, unsigned lane, int fd, uint64_t deadline) {
    (void)lane;
    atomic_uint *accepted = context;
    atomic_fetch_add_explicit(accepted, 1, memory_order_relaxed);
    kvf_frame f;
    while (kvf_recv_frame(fd, &f, NULL, 0, deadline) == KVF_OK) {
        f.response = 1;
        assert(kvf_send_frame(fd, f, NULL, deadline) == KVF_OK);
    }
}

static void peer_epoch(void) {
    atomic_uint accepted;
    atomic_init(&accepted, 0);
    kvf_server *server;
    kvf_endpoint loop = {{127, 0, 0, 1}, 0};
    assert(kvf_server_start(loop, 2, 1500, probe_peer, &accepted, &server) == KVF_OK);
    mi_peer_pool *pool;
    assert(mi_peer_pool_create(2, 200, &pool) == KVF_OK);
    kvf_grant grant = {.endpoint = kvf_server_endpoint(server)};
    assert(kvf_uuid_new(&grant.manifest.owner.boot) == KVF_OK);
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        if (attempt == 2)
            assert(kvf_uuid_new(&grant.manifest.owner.boot) == KVF_OK);
        if (attempt == 3) {
            assert(nanosleep(&(struct timespec){.tv_nsec = 230000000}, NULL) == 0);
            mi_peer_pool_reap(pool);
        }
        mi_peer_conn *peer;
        mi_remote_socket socket;
        int reused;
        uint64_t deadline = kvf_now_ms() + 1000;
        if (attempt == 1) {
            /* Even with the same endpoint and boot, a TCP stream can't satisfy an RDMA checkout. */
            kvf_status st = mi_peer_borrow(pool, &grant, KVF_TRANSPORT_RDMA_RSOCKET, kvf_now_ms(),
                                           &peer, &socket, &reused);
            assert((st == KVF_TIMEOUT || st == KVF_UNAVAILABLE) && !peer && !reused);
        }
        assert(mi_peer_borrow(pool, &grant, KVF_TRANSPORT_TCP, deadline, &peer, &socket, &reused) == KVF_OK);
        assert(reused == (attempt == 1));
        assert(socket.transport == KVF_TRANSPORT_TCP);
        assert(kvf_rpc_on(socket.fd, KVF_STATS, NULL, NULL, 0, NULL, deadline) == KVF_OK);
        mi_peer_return(pool, peer, socket, 1);
        assert(atomic_load_explicit(&accepted, memory_order_relaxed) == (attempt ? attempt : 1));
    }
    mi_peer_pool_destroy(pool);
    kvf_server_stop(server);
}

static void rdma_poll_deadline(void) {
    int ends[2];
    assert(pipe(ends) == 0);
    /* rpoll supports ordinary fds, so this exercises the actual library without an IB device. */
    mi_remote_socket input = {.fd = ends[0], .transport = KVF_TRANSPORT_RDMA_RSOCKET};
    struct pollfd ready = {.fd = input.fd, .events = POLLIN};
    int count = remote_poll(input.transport, &ready, 1, 0);
    assert(count == 0 || (count < 0 && errno == EOPNOTSUPP));
    if (!count) {
        for (int i = 0; i < 3; ++i)
            assert(remote_wait(input, POLLIN, kvf_now_ms() + 10) == KVF_TIMEOUT);
        assert(write(ends[1], "x", 1) == 1);
        assert(remote_wait(input, POLLIN, kvf_now_ms() + 100) == KVF_OK);
    }
    close(ends[0]);
    close(ends[1]);
}

int main(void) {
    exercise(DISCONNECT, "partial TCP response", KVF_IO);
    exercise(BAD_CRC, "layer CRC32C", KVF_CRC);
    exercise(EXPIRE, "expired directory lease", KVF_STALE);
    exercise(EVICT, "eviction and generation reuse", KVF_STALE);
    exercise(PAUSE, "prefetch cancellation and bounded queue", KVF_CANCELLED);
    leaf_pressure();
    peer_epoch();
    rdma_poll_deadline();
    return 0;
}
