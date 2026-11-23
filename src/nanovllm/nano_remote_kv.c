#include "nano_remote_kv.h"
#include "nano_cache.h"
#ifdef NANO_WITH_CUDA
#include "nano_cuda.h"
#endif
#include "miniinfer/remote_cache.h"
#include "kvfabric/wire.h"
#include <float.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(INT_MAX == INT32_MAX && CHAR_BIT == 8, "KV-Fabric token and byte width");

typedef struct NvRemoteExport {
    struct NvRemoteExport *next;
    unsigned char *snapshot;
    kvf_hash parent, hash;
    int queued, evicted, attempted;
    int32_t tokens[];
} NvRemoteExport;

typedef struct {
    NvRemoteExport *export;
    kvf_hash hash;
} NvRemoteSlot;

typedef struct NvRemoteRequest {
    struct NvRemoteRequest *next;
    mi_remote_prefetch *job;
    uint64_t deadline;
    int max_pages;
    uint64_t page_transfer_ns[];
} NvRemoteRequest;

typedef struct NvRemoteKV {
    mi_mempool *pool;
    mi_storage_backend *backend;
    mi_tiered_storage *storage;
    mi_remote_worker *worker;
    kvf_namespace space;
    size_t layer_half, page_half;
    unsigned capacity, timeout_ms;
    unsigned char *import_stage;
    NvRemoteSlot *slots;
    NvRemoteRequest *abandoned;
    NvRemoteStats stats;
    uint64_t *transfer_samples;
    size_t sample_capacity, sample_count;

    /* The queue mutex protects ownership, never socket I/O or CUDA transfers. */
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t publisher;
    NvRemoteExport *head, *tail;
    size_t queued, snapshots;
    int stopping;
} NvRemoteKV;

void nv_remote_options_defaults(NvRemoteOptions *o) {
    *o = (NvRemoteOptions){.tenant = "default", .prefetch_timeout_ms = 1000,
                          .tp_size = 1, .pp_size = 1};
}

static int tokenizer_digest(const NvTokenizer *t, kvf_hash *out) {
    static const char domain[] = "Nano-VLLM tokenizer BPE v1";
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (!md)
        return 0;
    uint32_t config[] = {(uint32_t)t->vocab_size, (uint32_t)t->max_length};
    int ok = EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1 &&
        EVP_DigestUpdate(md, domain, sizeof(domain) - 1) == 1 &&
        EVP_DigestUpdate(md, config, sizeof(config)) == 1;
    for (int i = 0; ok && i < t->vocab_size; ++i) {
        size_t bytes = strlen(t->vocab[i]);
        uint32_t length = (uint32_t)bytes;
        ok = bytes == length && EVP_DigestUpdate(md, &length, sizeof(length)) == 1 &&
            EVP_DigestUpdate(md, t->vocab[i], length) == 1 &&
            EVP_DigestUpdate(md, &t->scores[i], sizeof(t->scores[i])) == 1;
    }
    unsigned bytes;
    ok = ok && EVP_DigestFinal_ex(md, out->bytes, &bytes) == 1 && bytes == sizeof(out->bytes);
    EVP_MD_CTX_free(md);
    return ok;
}

static int make_identity(const NvEngine *e, const NvTokenizer *tokenizer, NvRemoteOptions o,
                         kvf_namespace *space) {
    const NvCache *cache = &e->cache;
    const NvConfig *model = &e->model->config;
    uint32_t endian = 1;
    if (*(const unsigned char *)&endian != 1 || sizeof(float) != 4 ||
        FLT_RADIX != 2 || FLT_MANT_DIG != 24 || FLT_MAX_EXP != 128)
        return -1;
    int head_dim = model->dim / model->n_heads;
    size_t layer = cache->elements / (size_t)cache->num_blocks / (size_t)cache->layers;
    if (cache->layers != model->n_layers || cache->kv_dim != head_dim * model->n_kv_heads ||
        layer / (size_t)cache->block_size != (size_t)cache->kv_dim ||
        layer % (size_t)cache->block_size || (unsigned)cache->block_size > KVF_MAX_TOKENS_PER_PAGE ||
        (unsigned)cache->layers > KVF_MAX_LAYERS || layer > KVF_MAX_LAYER_BYTES / (2 * sizeof(float)))
        return -1;

    *space = (kvf_namespace){.tokens_per_page = (uint32_t)cache->block_size,
        .layers = (uint32_t)cache->layers, .bytes_per_layer = (uint32_t)(2 * layer * sizeof(float)),
        .dtype = KVF_F32, .tp_size = 1, .pp_size = 1};
    static const char layout[] = "Nano-VLLM KV v1: layer K[token,head,dim],V[token,head,dim]; f32le";
    uint8_t canonical[sizeof(layout) + 6 * sizeof(uint32_t)];
    kvf_writer writer = {.buffer = canonical, .capacity = sizeof(canonical)};
    kvf_put_bytes(&writer, layout, sizeof(layout));
    kvf_put_u32(&writer, (uint32_t)model->n_kv_heads);
    kvf_put_u32(&writer, (uint32_t)head_dim);
    kvf_put_u32(&writer, space->layers);
    kvf_put_u32(&writer, space->tokens_per_page);
    kvf_put_u32(&writer, space->dtype);
#ifdef NV_USE_OPENBLAS
    kvf_put_u32(&writer, cache->cuda ? 2 : 1);
#else
    kvf_put_u32(&writer, cache->cuda ? 2 : 0);
#endif
    kvf_hash digest;
    return !writer.failed &&
        kvf_hash_bytes(e->model->mapping, e->model->mapping_size, &space->model) == KVF_OK &&
        tokenizer_digest(tokenizer, &space->tokenizer) &&
        kvf_hash_bytes(canonical, writer.size, &space->layout) == KVF_OK &&
        kvf_hash_bytes(o.tenant, strlen(o.tenant), &space->tenant) == KVF_OK &&
        kvf_identity_hash(space, &digest) == KVF_OK;
}

static int endpoint_specified(kvf_endpoint ep) {
    return ep.ipv4[0] || ep.ipv4[1] || ep.ipv4[2] || ep.ipv4[3];
}

static int configure_worker(mi_remote_config *cfg, NvRemoteOptions o) {
    mi_remote_config_defaults(cfg);
    if ((o.control_endpoint && kvf_endpoint_parse(o.control_endpoint, &cfg->control) != KVF_OK) ||
        (o.listen_endpoint && kvf_endpoint_parse(o.listen_endpoint, &cfg->listen) != KVF_OK) ||
        (o.advertise_endpoint && kvf_endpoint_parse(o.advertise_endpoint, &cfg->advertise) != KVF_OK) ||
        !cfg->control.port || !endpoint_specified(cfg->control) ||
        (!endpoint_specified(cfg->listen) && !endpoint_specified(cfg->advertise)) ||
        (o.advertise_endpoint && !endpoint_specified(cfg->advertise)))
        return -1;
    if (o.worker_name) {
        kvf_owner owner = {.boot = {{1}}};
        size_t length = strlen(o.worker_name);
        if (length >= sizeof(owner.name))
            return -1;
        memcpy(owner.name, o.worker_name, length + 1);
        if (!kvf_owner_valid(&owner))
            return -1;
        memcpy(cfg->worker_name, owner.name, sizeof(cfg->worker_name));
    } else {
        kvf_uuid uuid;
        if (kvf_uuid_new(&uuid) != KVF_OK)
            return 0;
        static const char hex[] = "0123456789abcdef";
        memcpy(cfg->worker_name, "nano-", 5);
        for (size_t i = 0; i < sizeof(uuid.bytes); ++i) {
            cfg->worker_name[5 + 2 * i] = hex[uuid.bytes[i] >> 4];
            cfg->worker_name[6 + 2 * i] = hex[uuid.bytes[i] & 15];
        }
    }
    cfg->max_exports = o.cache_capacity;
    cfg->timeout_ms = o.prefetch_timeout_ms;
    cfg->location = MI_LOCATION_CPU;
    return 1;
}

static int copy_block(NvCache *cache, int block, void *host, size_t half, int to_cache) {
#ifdef NANO_WITH_CUDA
    if (cache->cuda)
        return nv_cuda_copy_remote_block(cache, block, host, half, to_cache);
#endif
    nv_cache_copy_block(cache, block, host, half, to_cache);
    return 1;
}

static void write_export(NvRemoteKV *r, NvRemoteExport *item) {
    mi_kv_sequence *seq = NULL;
    if (mi_kv_sequence_create(mi_remote_worker_cache(r->worker), &seq) != MI_OK)
        return;
    mi_status st = mi_kv_sequence_append_page(seq, MI_LOCATION_CPU);
    for (uint32_t layer = 0; st == MI_OK && layer < r->space.layers; ++layer) {
        mi_page_lease lease;
        st = mi_paged_kv_acquire(seq, 0, layer, MI_ACCESS_WRITE, NULL, &lease);
        if (st != MI_OK)
            break;
        if (lease.lease.size != r->space.bytes_per_layer) {
            (void)mi_page_lease_abort(&lease);
            st = MI_ESTATE;
            break;
        }
        const unsigned char *key = item->snapshot + layer * r->layer_half;
        unsigned char *out = lease.lease.data;
        memcpy(out, key, r->layer_half);
        memcpy(out + r->layer_half, key + r->page_half, r->layer_half);
        st = mi_page_lease_finish(&lease);
        if (lease.kv)
            (void)mi_page_lease_abort(&lease);
    }
    if (st == MI_OK) {
        item->attempted = 1;
        (void)mi_remote_publish_page(r->worker, seq, item->parent, item->tokens);
    }
    mi_kv_sequence_destroy(seq);
}

/* Call with the queue mutex held. Eviction can reuse the publication item
 * without allocating, even when its original snapshot is still in flight. */
static void enqueue_export(NvRemoteKV *r, NvRemoteExport *item) {
    item->queued = 1;
    item->next = NULL;
    if (r->tail)
        r->tail->next = item;
    else
        r->head = item;
    r->tail = item;
    ++r->queued;
    pthread_cond_signal(&r->changed);
}

static void *publish_pages(void *arg) {
    NvRemoteKV *r = arg;
    pthread_mutex_lock(&r->lock);
    for (;;) {
        while (!r->head && !r->stopping)
            pthread_cond_wait(&r->changed, &r->lock);
        NvRemoteExport *item = r->head;
        if (!item)
            break;
        r->head = item->next;
        if (!r->head)
            r->tail = NULL;
        int has_snapshot = item->snapshot != NULL;
        int publish = has_snapshot && !item->evicted && !r->stopping;
        pthread_mutex_unlock(&r->lock);
        if (publish)
            write_export(r, item);
        free(item->snapshot);
        pthread_mutex_lock(&r->lock);
        if (has_snapshot) {
            item->snapshot = NULL;
            --r->snapshots;
        }
        if (item->evicted) {
            pthread_mutex_unlock(&r->lock);
            if (item->attempted)
                (void)mi_remote_invalidate_page(r->worker, item->hash);
            free(item);
            pthread_mutex_lock(&r->lock);
        } else {
            item->queued = 0;
        }
        if (!--r->queued)
            pthread_cond_broadcast(&r->changed);
    }
    pthread_mutex_unlock(&r->lock);
    return NULL;
}

static void destroy_remote(NvRemoteKV *r) {
    mi_remote_worker_stop(r->worker);
    mi_tiered_storage_destroy(r->storage);
    mi_backend_destroy(r->backend);
    mi_mempool_destroy(r->pool);
    free(r->import_stage);
    free(r->transfer_samples);
    free(r->slots);
    free(r);
}

int nv_engine_enable_remote(NvEngine *e, const NvTokenizer *tokenizer, NvRemoteOptions o) {
    if (!e || !e->model || !e->model->mapping || !e->model->mapping_size || !tokenizer ||
        !tokenizer->vocab || !tokenizer->scores || tokenizer->vocab_size != e->model->config.vocab_size ||
        !e->cache.enabled || e->remote || e->num_requests || !o.tenant || !*o.tenant ||
        o.tp_size != 1 || o.pp_size != 1 || !o.prefetch_timeout_ms || o.prefetch_timeout_ms > 30000 ||
        (o.page_size && o.page_size != (unsigned)e->cache.block_size))
        return -1;
    if (!o.cache_capacity)
        o.cache_capacity = (unsigned)e->cache.num_blocks;
    if (o.cache_capacity > 65536)
        return -1;
    mi_remote_config cfg;
    int configured = configure_worker(&cfg, o);
    if (configured != 1)
        return configured;
    configured = make_identity(e, tokenizer, o, &cfg.identity);
    if (configured != 1)
        return configured;

    /* Leave room for one complete staged request and a publication in flight.
     * A second staged request may fall back if the consumer has not collected it. */
    size_t pages = o.cache_capacity + (size_t)e->cache.num_blocks + 1;
    if (pages > UINT32_MAX / cfg.identity.layers)
        return -1;
    size_t blocks = pages * cfg.identity.layers;
    NvRemoteKV *r = calloc(1, sizeof(*r));
    if (!r)
        return 0;
    r->space = cfg.identity;
    r->capacity = o.cache_capacity;
    r->timeout_ms = o.prefetch_timeout_ms;
    r->layer_half = cfg.identity.bytes_per_layer / 2;
    r->page_half = r->layer_half * cfg.identity.layers;
    r->slots = calloc((size_t)e->cache.num_blocks, sizeof(*r->slots));
    r->import_stage = malloc(2 * r->page_half);
    r->sample_capacity = o.transfer_sample_capacity;
    if (r->sample_capacity)
        r->transfer_samples = calloc(r->sample_capacity, sizeof(*r->transfer_samples));
    if (!r->slots || !r->import_stage ||
        (r->sample_capacity && !r->transfer_samples) ||
        mi_mempool_create(MI_MEMORY_CPU, -1, (mi_mempool_config){0}, &r->pool) != MI_OK ||
        mi_memory_backend_create(MI_LOCATION_CPU, cfg.identity.bytes_per_layer, r->pool,
                                 blocks, &r->backend) != MI_OK)
        goto failed;
    mi_storage_config storage = {.block_bytes = cfg.identity.bytes_per_layer, .cpu_bytes = SIZE_MAX,
                                 .max_async_migrations = 1};
    if (mi_tiered_storage_create(storage, (mi_storage_backends){.cpu = r->backend}, &r->storage) != MI_OK ||
        mi_remote_worker_start(r->storage, cfg, &r->worker) != KVF_OK ||
        pthread_mutex_init(&r->lock, NULL) != 0)
        goto failed;
    if (pthread_cond_init(&r->changed, NULL) != 0)
        goto mutex_ready;
    if (pthread_create(&r->publisher, NULL, publish_pages, r) != 0)
        goto cond_ready;
    e->cache.remote = e->remote = r;
    return 1;

cond_ready:
    pthread_cond_destroy(&r->changed);
mutex_ready:
    pthread_mutex_destroy(&r->lock);
failed:
    destroy_remote(r);
    return 0;
}

static kvf_status prefix_hash(NvRemoteKV *r, const NvSequence *s, int pages, kvf_hash *out) {
    *out = (kvf_hash){0};
    int start = pages;
    while (start > 0) {
        kvf_hash known = r->slots[s->block_table[start - 1]].hash;
        if (!kvf_hash_zero(known)) {
            *out = known;
            break;
        }
        --start;
    }
    int n = (int)r->space.tokens_per_page;
    for (int i = start; i < pages; ++i) {
        kvf_status st = kvf_page_hash(&r->space, *out, s->tokens + i * n, (uint32_t)n, out);
        if (st != KVF_OK)
            return st;
        r->slots[s->block_table[i]].hash = *out;
    }
    return KVF_OK;
}

void nv_remote_publish(NvEngine *e, NvSequence *s) {
    NvRemoteKV *r = e->remote;
    int n = e->cache.block_size;
    int logical = s->computed / n - 1;
    int block = s->block_table[logical];
    NvRemoteSlot *slot = &r->slots[block];
    if (slot->export)
        return;
    pthread_mutex_lock(&r->lock);
    int room = r->snapshots < r->capacity;
    pthread_mutex_unlock(&r->lock);
    if (!room)
        return;

    kvf_hash parent, hash;
    if (prefix_hash(r, s, logical, &parent) != KVF_OK ||
        kvf_page_hash(&r->space, parent, s->tokens + logical * n, (uint32_t)n, &hash) != KVF_OK)
        return;
    slot->hash = hash;
    NvRemoteExport *item = calloc(1, sizeof(*item) + (size_t)n * sizeof(*item->tokens));
    if (!item)
        return;
    item->snapshot = malloc(2 * r->page_half);
    if (!item->snapshot || !copy_block(&e->cache, block, item->snapshot, r->page_half, 0)) {
        free(item->snapshot);
        free(item);
        return;
    }
    item->parent = parent;
    item->hash = hash;
    memcpy(item->tokens, s->tokens + logical * n, (size_t)n * sizeof(*item->tokens));
    pthread_mutex_lock(&r->lock);
    ++r->snapshots;
    slot->export = item;
    enqueue_export(r, item);
    pthread_mutex_unlock(&r->lock);
}

void nv_remote_evict(NvRemoteKV *r, int block) {
    NvRemoteSlot *slot = &r->slots[block];
    NvRemoteExport *item = slot->export;
    *slot = (NvRemoteSlot){0};
    if (!item)
        return;
    pthread_mutex_lock(&r->lock);
    item->evicted = 1;
    if (!item->queued)
        enqueue_export(r, item);
    pthread_mutex_unlock(&r->lock);
}

int nv_remote_begin(NvEngine *e, NvSequence *s) {
    NvRemoteKV *r = e->remote;
    if (s->remote_checked)
        return 0;
    s->remote_checked = 1;

    /* Retain a forward pass for the final prompt position to obtain logits. */
    int pages = (s->prompt_len - 1) / e->cache.block_size - s->num_blocks;
    if (pages <= 0)
        return 0;
    ++r->stats.remote_prefetch_requests;
    kvf_hash parent;
    size_t samples = r->sample_capacity ? (size_t)pages : 0;
    NvRemoteRequest *request = NULL;
    if (samples <= (SIZE_MAX - sizeof(*request)) / sizeof(*request->page_transfer_ns))
        request = calloc(1, sizeof(*request) + samples * sizeof(*request->page_transfer_ns));
    if (request && prefix_hash(r, s, s->num_blocks, &parent) == KVF_OK) {
        request->deadline = kvf_now_ms() + r->timeout_ms;
        request->max_pages = pages;
        size_t count = (size_t)pages * e->cache.block_size;
        mi_remote_suffix suffix = {.parent = parent, .deadline_ms = request->deadline,
            .page_transfer_ns = samples ? request->page_transfer_ns : NULL};
        if (mi_remote_prefetch_async_after(r->worker, suffix, s->tokens + s->computed,
                                          count, &request->job) == KVF_OK) {
            s->remote_request = request;
            return 1;
        }
    }
    free(request);
    ++r->stats.remote_prefetch_failures;
    ++r->stats.remote_prefill_fallbacks;
    return 0;
}

static void observe(NvRemoteKV *r, const NvRemoteRequest *request, mi_remote_result result) {
    r->stats.remote_lookup_requests += result.lookup_requests;
    r->stats.remote_lookup_hits += result.lookup_hits;
    r->stats.remote_transfer_latency_ns += result.transfer_ns;
    r->stats.remote_transfer_samples += result.transfer_samples;
    size_t left = r->sample_capacity - r->sample_count;
    size_t count = result.transfer_samples < left ? (size_t)result.transfer_samples : left;
    if (count) {
        memcpy(r->transfer_samples + r->sample_count, request->page_transfer_ns,
               count * sizeof(*r->transfer_samples));
        r->sample_count += count;
    }
}

static int read_stage(NvRemoteKV *r, mi_kv_sequence *seq, size_t page) {
    for (uint32_t layer = 0; layer < r->space.layers; ++layer) {
        mi_page_lease lease;
        if (mi_paged_kv_acquire(seq, page, layer, MI_ACCESS_READ, NULL, &lease) != MI_OK)
            return 0;
        int valid = lease.lease.size == r->space.bytes_per_layer;
        if (valid) {
            const unsigned char *in = lease.lease.data;
            unsigned char *key = r->import_stage + layer * r->layer_half;
            memcpy(key, in, r->layer_half);
            memcpy(key + r->page_half, in + r->layer_half, r->layer_half);
        }
        if (mi_page_lease_finish(&lease) != MI_OK)
            valid = 0;
        if (lease.kv)
            (void)mi_page_lease_abort(&lease);
        if (!valid)
            return 0;
    }
    return 1;
}

static int commit_pages(NvEngine *e, NvSequence *s, mi_kv_sequence *seq, int pages, uint64_t deadline) {
    NvRemoteKV *r = e->remote;
    NvCache *cache = &e->cache;
    int available = cache->free_count;
    /* SSD spill belongs to normal prefill. A remote reservation must finish
     * without waiting for a disk write or displacing a running request. */
    if (!cache->spill)
        for (int id = cache->oldest; id >= 0 && available < pages; id = cache->blocks[id].next)
            ++available;
    if (available < pages)
        return 0;

    int *staged = s->block_table + s->num_blocks;
    int allocated = 0;
    for (int page = 0; page < pages; ++page) {
        if (kvf_now_ms() >= deadline)
            break;
        int block = nv_cache_allocate(cache);
        if (block < 0)
            break;
        staged[allocated++] = block;
        if (!read_stage(r, seq, (size_t)page) ||
            !copy_block(cache, block, r->import_stage, r->page_half, 1))
            break;
        if (page + 1 == pages && kvf_now_ms() < deadline) {
            for (int i = 0; i < pages; ++i)
                nv_cache_commit_remote(cache, s, staged[i]);
            return 1;
        }
    }
    while (allocated)
        nv_cache_discard_remote(cache, staged[--allocated]);
    return 0;
}

static void abandon(NvRemoteKV *r, NvSequence *s) {
    NvRemoteRequest *request = s->remote_request;
    mi_remote_prefetch_cancel(request->job);
    request->next = r->abandoned;
    r->abandoned = request;
    s->remote_request = NULL;
}

int nv_remote_poll(NvEngine *e, NvSequence *s) {
    NvRemoteKV *r = e->remote;
    NvRemoteRequest *request = s->remote_request;
    mi_kv_sequence *seq = NULL;
    mi_remote_result result = {0};
    kvf_status status;
    if (!mi_remote_prefetch_poll(request->job, &seq, &result, &status)) {
        if (kvf_now_ms() < request->deadline)
            return 0;
        abandon(r, s);
        ++r->stats.remote_prefetch_failures;
        ++r->stats.remote_prefill_fallbacks;
        return 1;
    }
    observe(r, request, result);
    int pages = 0;
    size_t n = r->space.tokens_per_page;
    int usable = status == KVF_OK &&
        (result.remote_status == KVF_OK || result.remote_status == KVF_NOT_FOUND) &&
        seq && !(result.remote_tokens % n) &&
        result.remote_tokens / n <= (size_t)request->max_pages &&
        mi_kv_sequence_page_count(seq) == result.remote_tokens / n;
    if (usable)
        pages = (int)(result.remote_tokens / n);

    if (pages && commit_pages(e, s, seq, pages, request->deadline)) {
        r->stats.remote_hit_pages += (uint64_t)pages;
        r->stats.remote_hit_tokens += (uint64_t)pages * n;
        r->stats.remote_prefill_skipped_tokens += (uint64_t)pages * n;
        r->stats.remote_import_bytes += (uint64_t)pages * 2 * r->page_half;
    } else {
        ++r->stats.remote_prefill_fallbacks;
        if (!usable || pages || kvf_now_ms() >= request->deadline)
            ++r->stats.remote_prefetch_failures;
    }
    mi_kv_sequence_destroy(seq);
    mi_remote_prefetch_destroy(request->job);
    free(request);
    s->remote_request = NULL;
    return 1;
}

void nv_remote_cancel(NvEngine *e, NvSequence *s) {
    if (s->remote_request) {
        ++e->remote->stats.remote_prefetch_cancelled;
        abandon(e->remote, s);
    }
}

void nv_remote_reap(NvRemoteKV *r) {
    NvRemoteRequest **link = &r->abandoned;
    while (*link) {
        NvRemoteRequest *request = *link;
        mi_kv_sequence *seq = NULL;
        mi_remote_result result = {0};
        kvf_status status;
        if (mi_remote_prefetch_poll(request->job, &seq, &result, &status)) {
            *link = request->next;
            observe(r, request, result);
            mi_kv_sequence_destroy(seq);
            mi_remote_prefetch_destroy(request->job);
            free(request);
        } else {
            link = &request->next;
        }
    }
}

void nv_engine_remote_stats(const NvEngine *e, NvRemoteStats *out) {
    *out = e->remote ? e->remote->stats : (NvRemoteStats){0};
    if (e->remote) {
        kvf_worker_info info;
        mi_remote_worker_info(e->remote->worker, &info);
        out->remote_published_pages = info.published_pages;
        out->remote_cache_evictions = mi_remote_worker_evictions(e->remote->worker);
    }
}

const uint64_t *nv_engine_remote_take_transfer_samples(NvEngine *e, size_t *count) {
    NvRemoteKV *r = e->remote;
    *count = r ? r->sample_count : 0;
    if (!r)
        return NULL;
    r->sample_count = 0;
    return r->transfer_samples;
}

static void join_cancelled(NvRemoteKV *r) {
    while (r->abandoned) {
        NvRemoteRequest *request = r->abandoned;
        r->abandoned = request->next;
        mi_kv_sequence *seq = NULL;
        mi_remote_result result = {0};
        (void)mi_remote_prefetch_wait(request->job, &seq, &result);
        observe(r, request, result);
        mi_kv_sequence_destroy(seq);
        mi_remote_prefetch_destroy(request->job);
        free(request);
    }
}

void nv_engine_remote_flush(NvEngine *e) {
    NvRemoteKV *r = e->remote;
    if (!r)
        return;
    pthread_mutex_lock(&r->lock);
    while (r->queued)
        pthread_cond_wait(&r->changed, &r->lock);
    pthread_mutex_unlock(&r->lock);
    join_cancelled(r);
}

void nv_remote_close(NvEngine *e) {
    NvRemoteKV *r = e->remote;
    for (int i = 0; i < e->num_requests; ++i)
        nv_remote_cancel(e, e->requests[i]);
    pthread_mutex_lock(&r->lock);
    r->stopping = 1;
    pthread_cond_signal(&r->changed);
    pthread_mutex_unlock(&r->lock);
    join_cancelled(r);
    pthread_join(r->publisher, NULL);
    for (int i = 0; i < e->cache.num_blocks; ++i)
        free(r->slots[i].export);
    pthread_cond_destroy(&r->changed);
    pthread_mutex_destroy(&r->lock);
    e->cache.remote = e->remote = NULL;
    destroy_remote(r);
}
