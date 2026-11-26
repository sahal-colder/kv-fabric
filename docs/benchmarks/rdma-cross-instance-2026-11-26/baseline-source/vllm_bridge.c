#define _POSIX_C_SOURCE 200809L
#include "kvfabric/vllm_bridge.h"
#include "miniinfer/remote_cache.h"
#include <nvtx3/nvToolsExt.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct kvf_vllm_worker {
    mi_mempool *pool;
    mi_storage_backend *cpu, *disk;
    mi_tiered_storage *storage;
    mi_remote_worker *remote;
    kvf_identity identity;
    size_t window;
    unsigned read_lanes;
};

static kvf_status identity_for(const kvf_vllm_spec *spec, kvf_identity *identity) {
    static const char layout[] = "vLLM V1 BF16: each layer K,V[token,kv_head,head_dim]; le";
    if (!spec || !spec->control || !spec->model_key || !spec->tenant || !spec->block_tokens ||
        spec->block_tokens > KVF_MAX_TOKENS_PER_PAGE || !spec->layers ||
        spec->layers > KVF_MAX_LAYERS || !spec->layer_bytes ||
        spec->layer_bytes > KVF_MAX_LAYER_BYTES ||
        spec->layers * (size_t)spec->layer_bytes > KVF_MAX_BATCH_BYTES)
        return KVF_INVALID;
    *identity = (kvf_identity){.tokens_per_page = spec->block_tokens,
        .layers = spec->layers, .bytes_per_layer = spec->layer_bytes,
        .dtype = KVF_BF16, .tp_size = 1, .pp_size = 1};
    kvf_status st = kvf_hash_bytes(spec->model_key, strlen(spec->model_key), &identity->model);
    if (st == KVF_OK)
        st = kvf_hash_bytes(spec->tenant, strlen(spec->tenant), &identity->tenant);
    if (st == KVF_OK)
        st = kvf_hash_bytes(layout, sizeof(layout), &identity->layout);
    /* model_key includes the tokenizer and model configuration fingerprints. */
    identity->tokenizer = identity->model;
    return st;
}

kvf_status kvf_vllm_hashes(const kvf_vllm_spec *spec, const int32_t *tokens,
                          size_t pages, kvf_hash *out) {
    if (!tokens || !out)
        return KVF_INVALID;
    kvf_identity identity;
    kvf_status st = identity_for(spec, &identity);
    kvf_hash parent = {0};
    for (size_t i = 0; st == KVF_OK && i < pages; ++i) {
        st = kvf_page_hash(&identity, parent, tokens + i * spec->block_tokens,
                           spec->block_tokens, &out[i]);
        if (st == KVF_OK)
            parent = out[i];
    }
    return st;
}

kvf_status kvf_vllm_lookup(const kvf_vllm_spec *spec, const kvf_hash *hashes,
                          size_t pages, uint32_t timeout_ms, size_t *matched) {
    if (!hashes || !matched || !timeout_ms)
        return KVF_INVALID;
    *matched = 0;
    kvf_identity identity;
    kvf_status st = identity_for(spec, &identity);
    kvf_hash space;
    kvf_client client = {0};
    if (st == KVF_OK)
        st = kvf_identity_hash(&identity, &space);
    if (st == KVF_OK)
        st = kvf_endpoint_parse(spec->control, &client.endpoint);
    uint64_t deadline = kvf_now_ms() + timeout_ms;
    nvtxRangePushA("kvfabric.lookup");
    while (st == KVF_OK && *matched < pages) {
        uint64_t now = kvf_now_ms();
        if (now >= deadline) {
            st = KVF_TIMEOUT;
            break;
        }
        client.timeout_ms = (uint32_t)(deadline - now);
        size_t left = pages - *matched;
        uint32_t count = left < KVF_MAX_LOOKUP ? (uint32_t)left : KVF_MAX_LOOKUP;
        uint32_t found = 0;
        st = kvf_client_lookup_prefix(client, space, hashes + *matched, count, &found);
        *matched += found;
        if (found < count)
            break;
    }
    nvtxRangePop();
    return st;
}

void kvf_vllm_close(kvf_vllm_worker *w) {
    if (!w)
        return;
    mi_remote_worker_stop(w->remote);
    mi_tiered_storage_destroy(w->storage);
    mi_backend_destroy(w->disk);
    mi_backend_destroy(w->cpu);
    mi_mempool_destroy(w->pool);
    free(w);
}

kvf_status kvf_vllm_open_remote(const kvf_vllm_spec *spec, const char *name, const char *disk_path,
                        size_t cache_pages, uint32_t io_depth, const char *listen,
                        const char *transport, uint32_t timeout_ms, kvf_vllm_worker **out) {
    if (!out || !name || strnlen(name, KVF_WORKER_NAME) >= KVF_WORKER_NAME ||
        !cache_pages || cache_pages > UINT32_MAX || !io_depth || io_depth > 64)
        return KVF_INVALID;
    *out = NULL;
    kvf_identity identity;
    kvf_status st = identity_for(spec, &identity);
    mi_remote_config config;
    mi_remote_config_defaults(&config);
    if (st == KVF_OK)
        st = kvf_endpoint_parse(spec->control, &config.control);
    if (st == KVF_OK && listen)
        st = kvf_endpoint_parse(listen, &config.listen);
    if (st == KVF_OK && transport) {
        st = mi_remote_transport_parse(transport, &config.transport_mode);
        config.rdma_strict = 1;
    }
    if (timeout_ms) config.timeout_ms = timeout_ms;
    if (st != KVF_OK)
        return st;
    size_t page_bytes = identity.layers * (size_t)identity.bytes_per_layer;
    size_t window = KVF_MAX_BATCH_BYTES / page_bytes;
    if (window > KVF_MAX_BATCH_PAGES)
        window = KVF_MAX_BATCH_PAGES;
    size_t staging_pages = 2 * window + 1;
    size_t cpu_pages = staging_pages + (disk_path ? 0 : cache_pages);
    size_t stride = (identity.bytes_per_layer + 8191u) & ~(size_t)4095u;
    if (cpu_pages > UINT32_MAX / identity.layers || cache_pages >= UINT32_MAX / identity.layers)
        return KVF_LIMIT;

    kvf_vllm_worker *w = calloc(1, sizeof(*w));
    if (!w)
        return KVF_LIMIT;
    w->identity = identity;
    w->window = window;
    /* The background publisher needs a file slot even while all foreground
     * readers are active. Keep the total bounded by Direct's 64-slot limit. */
    w->read_lanes = disk_path ? (io_depth < 64 ? io_depth : 63) : 1;
    mi_status status = mi_mempool_create(MI_MEMORY_CPU, -1, (mi_mempool_config){0}, &w->pool);
    if (status == MI_OK)
        status = mi_memory_backend_create(MI_LOCATION_CPU, identity.bytes_per_layer,
                                          w->pool, cpu_pages * identity.layers, &w->cpu);
    size_t disk_bytes = disk_path ? (cache_pages + 1) * identity.layers * stride : 0;
    if (status == MI_OK && disk_path) {
        status = mi_ssd_direct_backend_create(disk_path, identity.bytes_per_layer, disk_bytes,
                                               MI_SSD_VERIFY_ON_READ, w->read_lanes + 1, -1, &w->disk);
        if (status == MI_OK && strncmp(mi_ssd_file_io_name(w->disk), "direct-", 7))
            status = MI_EUNSUPPORTED;
    }
    if (status == MI_OK) {
        mi_storage_config storage = {.block_bytes = identity.bytes_per_layer,
            .cpu_bytes = cpu_pages * identity.layers * mi_backend_allocation_bytes(w->cpu, identity.bytes_per_layer), .ssd_bytes = disk_bytes, .max_async_migrations = 1};
        status = mi_tiered_storage_create(storage,
            (mi_storage_backends){.cpu = w->cpu, .ssd = w->disk}, &w->storage);
    }
    if (status != MI_OK) {
        fprintf(stderr, "KV-Fabric vLLM storage: %s\n", mi_status_string(status));
        kvf_vllm_close(w);
        return KVF_UNAVAILABLE;
    }
    memcpy(config.worker_name, name, strlen(name) + 1);
    config.identity = identity;
    config.location = MI_LOCATION_CPU;
    config.max_exports = (uint32_t)cache_pages;
    st = mi_remote_worker_start(w->storage, config, &w->remote);
    if (st != KVF_OK) {
        kvf_vllm_close(w);
        return st;
    }
    *out = w;
    return KVF_OK;
}

const char *kvf_vllm_io_mode(const kvf_vllm_worker *w) {
    return !w ? NULL : w->disk ? mi_ssd_file_io_name(w->disk) : "cpu";
}

static mi_status copy_page(mi_kv_sequence *seq, size_t page, const kvf_identity *identity,
                           void *buffer, mi_access_mode access) {
    unsigned char *bytes = buffer;
    for (uint32_t l = 0; l < identity->layers; ++l) {
        mi_page_lease lease;
        mi_status st = mi_paged_kv_acquire(seq, page, l, access, NULL, &lease);
        if (st != MI_OK)
            return st;
        if (lease.lease.size != identity->bytes_per_layer) {
            (void)mi_page_lease_abort(&lease);
            return MI_ESTATE;
        }
        if (access == MI_ACCESS_WRITE)
            memcpy(lease.lease.data, bytes, lease.lease.size);
        else
            memcpy(bytes, lease.lease.data, lease.lease.size);
        st = mi_page_lease_finish(&lease);
        if (lease.kv)
            (void)mi_page_lease_abort(&lease);
        if (st != MI_OK)
            return st;
        bytes += identity->bytes_per_layer;
    }
    return MI_OK;
}

kvf_status kvf_vllm_publish(kvf_vllm_worker *w, const kvf_hash *previous,
                           const int32_t *tokens, size_t pages, const void *data) {
    if (!w || !previous || !tokens || !data || !pages || pages > w->window)
        return KVF_INVALID;
    kvf_hash parent = *previous;
    size_t stride = w->identity.layers * (size_t)w->identity.bytes_per_layer;
    kvf_status st = KVF_OK;
    nvtxRangePushA("kvfabric.publish");
    for (size_t i = 0; st == KVF_OK && i < pages; ++i) {
        const int32_t *page_tokens = tokens + i * w->identity.tokens_per_page;
        mi_kv_sequence *seq = NULL;
        mi_status local = mi_kv_sequence_create(mi_remote_worker_cache(w->remote), &seq);
        st = local == MI_OK ? mi_remote_append_page(w->remote, seq) : KVF_LIMIT;
        if (st == KVF_OK)
            local = copy_page(seq, 0, &w->identity,
                              (void *)((const unsigned char *)data + i * stride), MI_ACCESS_WRITE);
        if (st == KVF_OK && local == MI_OK && w->disk) {
            local = mi_kv_sequence_seal_page(seq, 0, page_tokens);
            mi_page_ref ref = {0};
            if (local == MI_OK)
                local = mi_kv_sequence_retain_page(seq, 0, &ref);
            for (uint32_t l = 0; local == MI_OK && l < w->identity.layers; ++l)
                local = mi_storage_migrate(w->storage, mi_page_ref_block(&ref, l), MI_LOCATION_SSD);
            mi_page_ref_release(&ref);
        }
        if (st == KVF_OK)
            st = local == MI_OK ? mi_remote_publish_page(w->remote, seq, parent, page_tokens) : KVF_IO;
        mi_kv_sequence_destroy(seq);
        if (st == KVF_OK)
            st = kvf_page_hash(&w->identity, parent, page_tokens, w->identity.tokens_per_page, &parent);
    }
    nvtxRangePop();
    return st;
}

kvf_status kvf_vllm_load(kvf_vllm_worker *w, const kvf_hash *parent,
                        const int32_t *tokens, size_t pages, uint32_t timeout_ms, void *data) {
    if (!w || !parent || !tokens || !data || !pages || pages > w->window)
        return KVF_INVALID;
    if (!timeout_ms)
        return KVF_TIMEOUT;
    size_t total = pages * w->identity.tokens_per_page;
    mi_remote_suffix suffix = {.parent = *parent, .deadline_ms = kvf_now_ms() + timeout_ms};
    mi_remote_prefetch *job = NULL;
    nvtxRangePushA("kvfabric.fetch");
    kvf_status st = mi_remote_prefetch_async_after(w->remote, suffix, tokens, total, &job);
    mi_kv_sequence *seq = NULL;
    mi_remote_result result = {0};
    if (st == KVF_OK) {
        st = mi_remote_prefetch_wait(job, &seq, &result);
        if (getenv("KVF_PERF_TRACE") && result.ready_ns) {
            fprintf(stderr, "KVF_FETCH {\"pages\":%zu,\"queue_ns\":%llu,\"completion_to_wait_return_ns\":%llu,\"crc_ns\":%llu,\"cpu_stage_ns\":%llu,\"commit_ns\":%llu,\"directory_ns\":%llu,\"lease_ns\":%llu,\"net_recv_ns\":%llu}\n",
                pages, (unsigned long long)result.timing.prefetch_queue_ns,
                (unsigned long long)(kvf_now_ns() - result.ready_ns),
                (unsigned long long)result.timing.crc_verify_ns,
                (unsigned long long)result.timing.cpu_stage_ns,
                (unsigned long long)result.timing.kv_commit_ns,
                (unsigned long long)result.timing.dir_lookup_ns,
                (unsigned long long)result.timing.lease_check_ns,
                (unsigned long long)result.timing.net_recv_ns);
        }
        mi_remote_prefetch_destroy(job);
        if (st == KVF_OK)
            st = result.remote_status;
        if (st == KVF_OK && result.remote_tokens != total)
            st = KVF_NOT_FOUND;
    }
    size_t stride = w->identity.layers * (size_t)w->identity.bytes_per_layer;
    nvtxRangePushA("kvfabric.fetch.bridge_stage");
    for (size_t i = 0; st == KVF_OK && i < pages; ++i)
        if (copy_page(seq, i, &w->identity, (unsigned char *)data + i * stride, MI_ACCESS_READ) != MI_OK)
            st = KVF_IO;
    nvtxRangePop();
    if (st == KVF_OK && kvf_now_ms() >= suffix.deadline_ms)
        st = KVF_TIMEOUT;
    mi_kv_sequence_destroy(seq);
    nvtxRangePop();
    return st;
}

/* Each reader pins a committed export before touching Storage. Direct's slots
 * own their registered buffers until CQE completion. A batch is visible only
 * after every reader has joined; on failure the caller retries the suffix. */
typedef struct {
    kvf_vllm_worker *worker;
    const kvf_hash *hashes;
    unsigned char *data;
    size_t pages, stride;
    uint64_t deadline;
    atomic_size_t next, first_failed;
} owned_read;

static void *read_owned_lane(void *arg) {
    owned_read *read = arg;
    for (;;) {
        size_t i = atomic_fetch_add_explicit(&read->next, 1, memory_order_relaxed);
        if (i >= read->pages)
            break;
        nvtxRangePushA("kvfabric.fetch.owned.page");
        kvf_status st = mi_remote_read_owned_page(read->worker->remote, read->hashes[i],
                          read->deadline, read->data + read->stride * i);
        nvtxRangePop();
        if (st != KVF_OK) {
            size_t first = atomic_load_explicit(&read->first_failed, memory_order_relaxed);
            while (i < first && !atomic_compare_exchange_weak_explicit(&read->first_failed,
                        &first, i, memory_order_relaxed, memory_order_relaxed)) {}
        }
    }
    return NULL;
}

size_t kvf_vllm_load_owned(kvf_vllm_worker *w, const kvf_hash *hashes,
                          size_t pages, uint32_t timeout_ms, void *data) {
    if (!w || !hashes || !data || !timeout_ms)
        return 0;
    uint64_t deadline = kvf_now_ms() + timeout_ms;
    size_t stride = w->identity.layers * (size_t)w->identity.bytes_per_layer;
    size_t loaded = 0;
    nvtxRangePushA("kvfabric.fetch.owned");
    if (w->disk) {
        owned_read read = {.worker = w, .hashes = hashes, .data = data,
            .pages = pages, .stride = stride, .deadline = deadline};
        atomic_init(&read.next, 0);
        atomic_init(&read.first_failed, pages);
        pthread_t readers[63];
        unsigned count = 0;
        unsigned lanes = pages < w->read_lanes ? (unsigned)pages : w->read_lanes;
        for (unsigned i = 1; i < lanes; ++i) {
            if (pthread_create(&readers[count], NULL, read_owned_lane, &read))
                break; /* The calling thread drains the remaining work. */
            ++count;
        }
        read_owned_lane(&read);
        nvtxRangePushA("kvfabric.fetch.owned.join");
        for (unsigned i = 0; i < count; ++i)
            pthread_join(readers[i], NULL);
        nvtxRangePop();
        loaded = atomic_load_explicit(&read.first_failed, memory_order_relaxed);
        nvtxRangePop();
        return loaded;
    }
    while (loaded < pages &&
           mi_remote_read_owned_page(w->remote, hashes[loaded], deadline,
                                     (unsigned char *)data + stride * loaded) == KVF_OK)
        ++loaded;
    nvtxRangePop();
    return loaded;
}

kvf_status kvf_vllm_open(const kvf_vllm_spec *spec, const char *name, const char *disk,
                        size_t pages, uint32_t depth, kvf_vllm_worker **out) {
    return kvf_vllm_open_remote(spec, name, disk, pages, depth, NULL, NULL, 0, out);
}

kvf_status kvf_vllm_load_direct(kvf_vllm_worker *w, const kvf_hash *parent,
                       const int32_t *tokens, size_t pages, uint32_t timeout_ms, void *data) {
    if (!w || !parent || !tokens || !data || !pages || pages > 128) return KVF_INVALID;
    if (!timeout_ms) return KVF_TIMEOUT;
    mi_remote_timing t = {0};
    nvtxRangePushA("kvfabric.rdma.direct_receive");
    kvf_status st = mi_remote_snapshot_into(w->remote, *parent, tokens, pages,
                              kvf_now_ms() + timeout_ms, data, &t);
    nvtxRangePop();
    if (getenv("KVF_PERF_TRACE"))
        fprintf(stderr, "KVF_DIRECT {\"status\":%d,\"pages\":%zu,\"directory_ns\":%llu,\"connection_ns\":%llu,\"net_recv_ns\":%llu,\"crc_ns\":%llu,\"lease_ns\":%llu,\"received_bytes\":%llu,\"data_requests\":%llu,\"transport\":\"%s\"}\n",
                st, pages, (unsigned long long)t.dir_lookup_ns, (unsigned long long)t.conn_acquire_ns,
                (unsigned long long)t.net_recv_ns, (unsigned long long)t.crc_verify_ns,
                (unsigned long long)t.lease_check_ns, (unsigned long long)t.received_bytes,
                (unsigned long long)t.data_requests,
                mi_remote_transport_name(mi_remote_worker_active_transport(w->remote)));
    return st;
}
