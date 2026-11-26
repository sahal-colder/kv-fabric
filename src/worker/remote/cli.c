/*
 * Worker CLI and tensor-file boundary. CLI owns Storage, files and its I/O
 * segment. File/DMA calls use explicit page Leases with metadata locks released.
 */
#define _POSIX_C_SOURCE 200809L
#include "miniinfer/remote_cache.h"
#include "miniinfer/device_transfer.h"
#include "kvfabric/wire.h"
#include "../segment_pool.h"
#include "../tiered_internal.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int parse_number(const char *text, uint32_t *out) {
    if (*text < '0' || *text > '9')
        return 0;
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || *end || value > UINT32_MAX)
        return 0;
    *out = (uint32_t)value;
    return 1;
}

static int read_identity(const char *path, kvf_identity *id) {
    FILE *file = fopen(path, "r");
    if (!file)
        return 0;
    kvf_hash *hashes[] = {&id->model, &id->tokenizer, &id->layout, &id->lora, &id->tenant};
    uint32_t shape[8];
    char word[65];
    int ok = 1;
    for (size_t i = 0; ok && i < sizeof(hashes) / sizeof(*hashes); ++i)
        ok = fscanf(file, "%64s", word) == 1 && kvf_hash_parse(word, hashes[i]) == KVF_OK;
    for (size_t i = 0; ok && i < sizeof(shape) / sizeof(*shape); ++i)
        ok = fscanf(file, "%64s", word) == 1 && parse_number(word, &shape[i]) &&
             (i < 3 || shape[i] <= UINT16_MAX);
    if (!ok || fscanf(file, "%1s", word) != EOF || ferror(file)) {
        fclose(file);
        return 0;
    }
    fclose(file);
    id->tokens_per_page = shape[0];
    id->layers = shape[1];
    id->bytes_per_layer = shape[2];
    id->dtype = (uint16_t)shape[3];
    id->tp_rank = (uint16_t)shape[4];
    id->tp_size = (uint16_t)shape[5];
    id->pp_rank = (uint16_t)shape[6];
    id->pp_size = (uint16_t)shape[7];
    kvf_hash space;
    return kvf_identity_hash(id, &space) == KVF_OK;
}

static int32_t *read_tokens(const char *path, size_t *count) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    long bytes = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    int32_t *tokens = NULL;
    if (bytes <= 0 || bytes % 4 || (unsigned long)bytes / 4 > KVF_MAX_TOKENS_PER_PAGE * 65536u ||
        fseek(file, 0, SEEK_SET) != 0)
        goto done;
    *count = (size_t)bytes / 4;
    tokens = malloc(*count * sizeof(*tokens));
    if (!tokens)
        goto done;
    for (size_t i = 0; i < *count; ++i) {
        uint8_t word[4];
        if (fread(word, 1, sizeof(word), file) != sizeof(word)) {
            free(tokens);
            tokens = NULL;
            break;
        }
        kvf_reader r = {.buffer = word, .size = sizeof(word)};
        uint32_t token = kvf_get_u32(&r);
        if (token > INT32_MAX) {
            free(tokens);
            tokens = NULL;
            break;
        }
        tokens[i] = (int32_t)token;
    }
done:
    fclose(file);
    return tokens;
}

static mi_status layer_io(FILE *file, mi_kv_sequence *sequence, size_t page, uint32_t layer,
                          int load, int device, mi_stream stream, void *staging) {
    mi_page_lease lease;
    mi_status st = mi_paged_kv_acquire(sequence, page, layer, load ? MI_ACCESS_WRITE : MI_ACCESS_READ,
                                       stream, &lease);
    if (st != MI_OK)
        return st;
    size_t size = lease.lease.size;
    void *host = stream ? staging : lease.lease.data;
    if (load) {
        if (fread(host, 1, size, file) != size)
            st = MI_EIO;
        if (st == MI_OK && stream)
            st = mi_device_copy_from_host(device, lease.lease.data, host, size, stream);
    } else {
        if (stream) {
            st = mi_device_copy_to_host(device, host, lease.lease.data, size, stream);
            if (st == MI_OK)
                st = mi_device_stream_wait(device, stream);
        }
        if (st == MI_OK && fwrite(host, 1, size, file) != size)
            st = MI_EIO;
    }
    mi_status done = st == MI_OK ? mi_page_lease_finish(&lease) : mi_page_lease_abort(&lease);
    return st == MI_OK ? done : st;
}

static mi_status tensor_io(const char *path, mi_kv_sequence *sequence, size_t pages, const mi_remote_config *c,
                           int load, int device, mi_stream stream, void *staging) {
    FILE *file = fopen(path, load ? "rb" : "wb");
    if (!file)
        return MI_EIO;
    mi_status st = MI_OK;
    for (size_t page = 0; st == MI_OK && page < pages; ++page) {
        if (load)
            st = mi_kv_sequence_append_page(sequence, c->location);
        for (uint32_t layer = 0; st == MI_OK && layer < c->identity.layers; ++layer)
            st = layer_io(file, sequence, page, layer, load, device, stream, staging);
    }
    if (load && st == MI_OK && (fgetc(file) != EOF || ferror(file)))
        st = MI_EINVAL;
    return fclose(file) == 0 ? st : MI_EIO;
}

static void usage(FILE *out) {
    fputs("usage: miniinfer_worker serve|pull NAME CONTROL LISTEN cpu|cuda[:ID] NAMESPACE TOKENS TENSORS\n"
          "       [--kv-transport tcp|rdma] [--kv-rdma-fallback | --no-kv-rdma-fallback]\n", out);
}

static uint64_t monotonic_us(void) {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0
        ? (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u : 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        usage(stdout);
        return 0;
    }
    if (argc < 9 || (strcmp(argv[1], "serve") && strcmp(argv[1], "pull"))) {
        usage(stderr);
        return 2;
    }

    int load = !strcmp(argv[1], "serve");
    int device = -1;
    if (strcmp(argv[5], "cpu")) {
        uint32_t ordinal = 0;
        if (strcmp(argv[5], "cuda") &&
            (strncmp(argv[5], "cuda:", 5) || !parse_number(argv[5] + 5, &ordinal) || ordinal > INT32_MAX)) {
            usage(stderr);
            return 2;
        }
        if (mi_device_backend() != MI_DEVICE_CUDA) {
            fprintf(stderr, "miniinfer_worker: CUDA is unavailable in this build\n");
            return 2;
        }
        device = (int)ordinal;
    }
    mi_remote_config c;
    mi_remote_config_defaults(&c);
    for (int i = 9; i < argc; ++i) {
        if (!strcmp(argv[i], "--kv-rdma-fallback")) {
            c.rdma_strict = 0;
            continue;
        }
        if (!strcmp(argv[i], "--no-kv-rdma-fallback")) {
            c.rdma_strict = 1;
            continue;
        }
        if (!strcmp(argv[i], "--kv-transport") && i + 1 < argc &&
            mi_remote_transport_parse(argv[++i], &c.transport_mode) == KVF_OK)
            continue;
        usage(stderr);
        return 2;
    }
    const char *lanes = getenv("MINIINFER_WORKER_LANES");
    const char *peers = getenv("MINIINFER_WORKER_PEERS");
    const char *advertise = getenv("MINIINFER_WORKER_ADVERTISE");
    const char *ssd_path = getenv("MINIINFER_WORKER_SSD");
    if (strlen(argv[2]) >= sizeof(c.worker_name) ||
        kvf_endpoint_parse(argv[3], &c.control) != KVF_OK ||
        kvf_endpoint_parse(argv[4], &c.listen) != KVF_OK || !read_identity(argv[6], &c.identity) ||
        (lanes && (!parse_number(lanes, &c.lanes) || !c.lanes || c.lanes > 64)) ||
        (peers && (!parse_number(peers, &c.peer_connections) || c.peer_connections > 64)) ||
        (advertise && kvf_endpoint_parse(advertise, &c.advertise) != KVF_OK)) {
        usage(stderr);
        return 2;
    }
    memcpy(c.worker_name, argv[2], strlen(argv[2]) + 1);
    c.location = device < 0 ? MI_LOCATION_CPU : MI_LOCATION_GPU;

    sigset_t signals;
    if (sigemptyset(&signals) || sigaddset(&signals, SIGINT) || sigaddset(&signals, SIGTERM) ||
        pthread_sigmask(SIG_BLOCK, &signals, NULL))
        return 1;
    size_t count;
    int32_t *tokens = read_tokens(argv[7], &count);
    if (!tokens || count % c.identity.tokens_per_page) {
        free(tokens);
        return 2;
    }
    size_t pages = count / c.identity.tokens_per_page;
    size_t stride = ((size_t)c.identity.bytes_per_layer + 255) & ~(size_t)255;
    size_t disk_stride = (((size_t)c.identity.bytes_per_layer + 4095) & ~(size_t)4095) + 4096;
    if (pages > 65536 || pages > SIZE_MAX / c.identity.layers / stride ||
        (ssd_path && pages > SIZE_MAX / c.identity.layers / disk_stride)) {
        free(tokens);
        return 2;
    }
    c.max_exports = (uint32_t)pages;

    int rc = 1;
    const char *operation = "create memory pool";
    mi_mempool *pool = NULL;
    mi_segment_pool *file_pool = NULL;
    mi_segment *file_stage = NULL;
    mi_storage_backend *backend = NULL, *ssd = NULL;
    mi_tiered_storage *storage = NULL;
    mi_remote_worker *worker = NULL;
    mi_kv_sequence *seq = NULL;
    mi_stream stream = NULL;
    void *stage = NULL;
    size_t slots = pages * c.identity.layers;
    size_t budget = slots * stride;
    mi_mempool_config pool_cfg = {.alignment = 64, .max_reserved_bytes = budget};
    mi_status st = mi_mempool_create(device < 0 ? MI_MEMORY_CPU : MI_MEMORY_DEVICE, device, pool_cfg, &pool);
    if (st != MI_OK)
        goto done;
    operation = "create storage backend";
    st = mi_memory_backend_create(c.location, c.identity.bytes_per_layer, pool, slots, &backend);
    if (st != MI_OK)
        goto done;
    mi_storage_config cfg;
    mi_storage_config_default(&cfg);
    cfg.block_bytes = c.identity.bytes_per_layer;
    cfg.cpu_bytes = 0;
    mi_storage_backends backends = {0};
    if (device < 0) {
        backends.cpu = backend;
        cfg.cpu_bytes = budget;
    } else {
        operation = "create accelerator file staging";
        backends.gpu = backend;
        cfg.gpu_bytes = budget;
        st = mi_segment_pool_create(device, stride, 1, 1, &file_pool);
        if (st != MI_OK)
            goto done;
        file_stage = mi_segment_acquire(file_pool, 0, 1);
        stream = file_stage->stream;
        stage = file_stage->data;
    }
    if (ssd_path) {
        operation = "open aligned SSD cache file";
        cfg.ssd_bytes = slots * disk_stride;
        st = mi_ssd_direct_backend_create(ssd_path, c.identity.bytes_per_layer, cfg.ssd_bytes,
                                          MI_SSD_VERIFY_ON_READ, c.lanes, device, &ssd);
        if (st != MI_OK)
            goto done;
        backends.ssd = ssd;
    }
    operation = "create tiered storage";
    st = mi_tiered_storage_create(cfg, backends, &storage);
    if (st != MI_OK)
        goto done;
    operation = "start Worker";
    kvf_status remote = mi_remote_worker_start(storage, c, &worker);
    if (remote != KVF_OK)
        goto remote_error;

    if (load) {
        operation = "load layers";
        st = mi_kv_sequence_create(mi_remote_worker_cache(worker), &seq);
        if (st == MI_OK)
            st = tensor_io(argv[8], seq, pages, &c, 1, device, stream, stage);
        if (st != MI_OK)
            goto done;
        if (ssd) {
            operation = "seal pages for SSD migration";
            st = mi_kv_sequence_publish(seq, tokens, count, pages);
            if (st != MI_OK)
                goto done;
            /* Advertise only after the disk-backed snapshot can serve every layer. */
            operation = "migrate sealed pages to SSD";
            for (size_t page = 0; st == MI_OK && page < pages; ++page) {
                mi_page_ref ref;
                st = mi_kv_sequence_retain_page(seq, page, &ref);
                if (st != MI_OK)
                    break;
                for (uint32_t i = 0; st == MI_OK && i < c.identity.layers; ++i)
                    st = mi_storage_migrate(storage, mi_page_ref_block(&ref, i), MI_LOCATION_SSD);
                mi_page_ref_release(&ref);
            }
            if (st != MI_OK)
                goto done;
        }
        operation = "publish pages";
        remote = mi_remote_publish(worker, seq, tokens, count, pages);
        if (remote != KVF_OK)
            goto remote_error;
        mi_kv_sequence_destroy(seq);
        seq = NULL;
        kvf_worker_info info;
        char endpoint[KVF_ENDPOINT_TEXT];
        mi_remote_worker_info(worker, &info);
        kvf_endpoint_format(info.endpoint, endpoint);
        fprintf(stderr, "active_transport: %s\n", mi_remote_transport_name(mi_remote_worker_active_transport(worker)));
        printf("READY %s %u\n", endpoint, info.published_pages);
        int signal_number;
        rc = fflush(stdout) != 0 || sigwait(&signals, &signal_number) != 0;
    } else {
        operation = "resolve remote pages";
        mi_remote_result result;
        uint64_t begin = monotonic_us();
        remote = mi_remote_resolve(worker, tokens, count, &seq, &result);
        uint64_t elapsed = monotonic_us() - begin;
        if (remote != KVF_OK)
            goto remote_error;
        fprintf(stderr, "active_transport: %s\n", mi_remote_transport_name(mi_remote_worker_active_transport(worker)));
        size_t verified = mi_kv_sequence_page_count(seq);
        operation = "write verified layers";
        st = tensor_io(argv[8], seq, verified, &c, 0, device, stream, stage);
        if (st != MI_OK)
            goto done;
        mi_kv_sequence_destroy(seq);
        seq = NULL;
        mi_remote_result local;
        operation = "resolve cached pages";
        remote = mi_remote_resolve(worker, tokens, verified * c.identity.tokens_per_page, &seq, &local);
        if (remote != KVF_OK)
            goto remote_error;
        printf("PULL pages=%zu local_tokens=%zu remote_tokens=%zu cached_tokens=%zu status=%s\n",
               verified, result.local_tokens, result.remote_tokens, local.local_tokens,
               kvf_status_string(result.remote_status));
        rc = verified == pages && local.local_tokens == count ? 0 : 3;
        if (!rc && getenv("MINIINFER_WORKER_TIMING"))
            fprintf(stderr, "TRANSFER %llu %zu\n", (unsigned long long)elapsed,
                    verified * c.identity.layers * c.identity.bytes_per_layer);
    }
    goto done;

remote_error:
    fprintf(stderr, "miniinfer_worker: %s: %s\n", operation, kvf_status_string(remote));
done:
    if (st != MI_OK)
        fprintf(stderr, "miniinfer_worker: %s: %s\n", operation, mi_status_string(st));
    mi_kv_sequence_destroy(seq);
    mi_remote_worker_stop(worker);
    if (storage && mi_storage_quarantined(storage))
        mi_segment_quarantine(file_stage);
    mi_segment_release(file_stage);
    mi_segment_pool_destroy(file_pool);
    mi_tiered_storage_destroy(storage);
    mi_backend_destroy(ssd);
    mi_backend_destroy(backend);
    mi_mempool_destroy(pool);
    free(tokens);
    return rc;
}
