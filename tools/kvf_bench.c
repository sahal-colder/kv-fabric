/*
 * Reproducible Storage and Peer benchmarks. A batch owns workers, slots, threads
 * and output files. The start gate is independent of production metadata locks.
 */
#define _GNU_SOURCE
#include "fixture.h"
#include "../src/worker/remote/internal.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

enum { MAX_CLIENTS = 32, WARMUP = 4, SAMPLES = 16, ROUNDS = 3 };
enum { CLIENT, DIRECTORY, SOURCE, PROCESSES };

typedef struct {
    kvf_client control;            /* External Directory for TCP runs. */
    kvf_page_key key;              /* Stable first page of the FP32 fixture. */
    mi_storage_backend *backend;   /* Resident tier for TCP, file tier for SSD. */
    mi_mempool *pool;               /* Owns resident allocation when benchmarking TCP. */
    mi_tiered_storage *storage;     /* TCP receiver's Storage. */
    mi_remote_worker *worker;      /* Uses production peers and DMA import. */
    mi_segment_pool *verify;       /* Reusable, per-client read-back buffer. */
    float *expected;               /* Read-only fixed data set shared by clients. */
    size_t bytes;                  /* Layer or disk record payload size. */
    unsigned clients, samples;    /* Concurrent threads and samples per thread. */
    const char *mode, *device_name;
    int device, tcp;
    pthread_mutex_t lock;          /* Protects only the start gate. */
    pthread_cond_t ready;
    int go;
} batch;

typedef struct {
    batch *b;
    unsigned id;
    kvf_status status;
    uint64_t latency[SAMPLES];
} lane;

typedef struct {
    clockid_t clock[PROCESSES];
    uint64_t elapsed, cpu[PROCESSES];
    int pid[PROCESSES];
} counters;

static uint64_t time_ns(clockid_t id) {
    struct timespec ts;
    return clock_gettime(id, &ts) == 0 ? (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec : 0;
}

static kvf_status tcp_request(lane *l, mi_segment *check) {
    batch *b = l->b;
    kvf_grant grant;
    kvf_status st = kvf_client_acquire(b->control, b->key, &grant);
    if (st != KVF_OK)
        return st;
    mi_kv_sequence *sequence = NULL;
    st = mi_remote_from_storage(mi_kv_sequence_create(mi_remote_worker_cache(b->worker), &sequence));
    if (st == KVF_OK)
        st = mi_remote_append_page(b->worker, sequence);
    if (st == KVF_OK)
        st = mi_remote_import(b->worker, sequence, 0, &grant, NULL);
    kvf_status finished = kvf_client_release(b->control, grant.ticket);
    if (st == KVF_OK)
        st = finished;
    for (unsigned i = 0; st == KVF_OK && i < LAYERS; ++i) {
        mi_page_lease lease;
        mi_status local = mi_paged_kv_acquire(sequence, 0, i, MI_ACCESS_READ, check->stream, &lease);
        if (local != MI_OK) {
            st = mi_remote_from_storage(local);
            break;
        }
        const void *data = lease.lease.data;
        if (check->stream) {
            local = mi_device_copy_to_host(b->device, check->data, data, b->bytes, check->stream);
            if (local == MI_OK)
                local = mi_device_stream_wait(b->device, check->stream);
            data = check->data;
        }
        if (local == MI_OK && memcmp(data, (const unsigned char *)b->expected + i * b->bytes, b->bytes))
            local = MI_ECRC;
        finished = mi_remote_from_storage(mi_page_lease_finish(&lease));
        st = local == MI_OK ? finished : mi_remote_from_storage(local);
    }
    /* The benchmark imports to private pages to avoid measuring a local Trie hit. */
    mi_kv_sequence_destroy(sequence);
    return st;
}

static kvf_status ssd_request(lane *l, mi_segment *check, unsigned sample) {
    batch *b = l->b;
    mi_block_meta record = {.id = l->id + 1, .version = sample + 1, .size = b->bytes,
                            .location = MI_LOCATION_SSD};
    mi_storage_handle slot;
    mi_status st = mi_backend_reserve(b->backend, b->bytes, &slot);
    if (st != MI_OK)
        return mi_remote_from_storage(st);
    st = mi_backend_write_record(b->backend, slot, &record, b->expected);
    if (st == MI_OK)
        st = mi_backend_read_validated(b->backend, slot, &record, check->data);
    if (st == MI_OK && memcmp(b->expected, check->data, b->bytes))
        st = MI_ECRC;
    mi_status done = mi_backend_release(b->backend, slot);
    return mi_remote_from_storage(st == MI_OK ? done : st);
}

static void *run_lane(void *arg) {
    lane *l = arg;
    batch *b = l->b;
    pthread_mutex_lock(&b->lock);
    while (!b->go)
        pthread_cond_wait(&b->ready, &b->lock);
    pthread_mutex_unlock(&b->lock);
    mi_segment *check = mi_segment_acquire(b->verify, l->id, l->id + 1);
    l->status = KVF_LIMIT;
    if (!check)
        return NULL;
    for (unsigned i = 0; i < b->samples; ++i) {
        uint64_t start = time_ns(CLOCK_MONOTONIC);
        l->status = b->tcp ? tcp_request(l, check) : ssd_request(l, check, i);
        l->latency[i] = time_ns(CLOCK_MONOTONIC) - start;
        if (l->status != KVF_OK)
            break;
    }
    mi_segment_release(check);
    return NULL;
}

static int run_batch(batch *b, lane *lanes, unsigned samples, counters *c) {
    pthread_t threads[MAX_CLIENTS];
    uint64_t start[PROCESSES] = {0};
    b->go = 0;
    b->samples = samples;
    unsigned started = 0;
    for (; started < b->clients; ++started)
        if (pthread_create(&threads[started], NULL, run_lane, &lanes[started]) != 0)
            break;
    for (size_t i = 0; i < PROCESSES; ++i)
        if (c->pid[i])
            start[i] = time_ns(c->clock[i]);
    pthread_mutex_lock(&b->lock);
    uint64_t wall = time_ns(CLOCK_MONOTONIC);
    b->go = 1;
    pthread_cond_broadcast(&b->ready);
    pthread_mutex_unlock(&b->lock);
    for (unsigned i = 0; i < started; ++i)
        pthread_join(threads[i], NULL);
    c->elapsed = time_ns(CLOCK_MONOTONIC) - wall;
    for (size_t i = 0; i < PROCESSES; ++i) {
        if (!c->pid[i])
            continue;
        uint64_t end = time_ns(c->clock[i]);
        if (end < start[i] || !end)
            return 0;
        c->cpu[i] = end - start[i];
    }
    for (unsigned i = 0; i < started; ++i) {
        if (lanes[i].status != KVF_OK) {
            fprintf(stderr, "%s/%s client %u: %s\n", b->device_name, b->mode, i,
                    kvf_status_string(lanes[i].status));
            return 0;
        }
    }
    return started == b->clients && c->elapsed > 0;
}

static size_t rss(int pid) {
    char path[48], line[256];
    if (!pid)
        return 0;
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *file = fopen(path, "r");
    if (!file)
        return 0;
    size_t kib = 0;
    while (fgets(line, sizeof(line), file))
        if (sscanf(line, "VmHWM: %zu kB", &kib) == 1)
            break;
    fclose(file);
    return kib;
}

static int ascending(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static double percentile(const uint64_t *ns, size_t n, unsigned p) {
    return (double)ns[(n * p + 99) / 100 - 1] / 1000.0;
}

static int write_round(FILE *raw, FILE *summary, const batch *b, unsigned round,
                        const lane *lanes, const counters *c) {
    uint64_t ns[MAX_CLIENTS * SAMPLES];
    size_t count = 0;
    for (unsigned i = 0; i < b->clients; ++i) {
        for (unsigned j = 0; j < SAMPLES; ++j) {
            uint64_t duration = lanes[i].latency[j];
            if (fprintf(raw, "%s,%s,%zu,%u,%u,%u,%u,%" PRIu64 "\n", b->mode, b->device_name,
                        b->bytes, b->clients, round, i, j, duration) < 0)
                return 0;
            ns[count++] = duration;
        }
    }
    qsort(ns, count, sizeof(*ns), ascending);
    /* TCP moves two layers in one direction. SSD moves one layer in each direction. */
    double mibps = (double)b->bytes * count * 2e9 / (1048576.0 * c->elapsed);
    return fprintf(summary,
                   "%s,%s,%zu,%u,%u,%zu,%" PRIu64 ",%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%zu,%zu,%zu\n",
                   b->mode, b->device_name, b->bytes, b->clients, round, count, c->elapsed, mibps,
                   percentile(ns, count, 50), percentile(ns, count, 95), percentile(ns, count, 99),
                   c->cpu[CLIENT] / 1.0e6, c->cpu[DIRECTORY] / 1.0e6, c->cpu[SOURCE] / 1.0e6,
                   rss(c->pid[CLIENT]), rss(c->pid[DIRECTORY]), rss(c->pid[SOURCE])) > 0;
}

static int measure(batch *b, counters c, const char *raw_path, const char *summary_path) {
    c.pid[CLIENT] = getpid();
    for (size_t i = 0; i < PROCESSES; ++i)
        if (c.pid[i] && clock_getcpuclockid(c.pid[i], &c.clock[i]))
            return 0;
    if (pthread_mutex_init(&b->lock, NULL))
        return 0;
    if (pthread_cond_init(&b->ready, NULL)) {
        pthread_mutex_destroy(&b->lock);
        return 0;
    }
    FILE *raw = fopen(raw_path, "a"), *summary = fopen(summary_path, "a");
    b->expected = malloc(b->bytes * LAYERS);
    int ok = raw && summary && b->expected &&
        mi_segment_pool_create(b->device, b->bytes, b->clients, 1, &b->verify) == MI_OK;
    lane lanes[MAX_CLIENTS] = {0};
    if (ok) {
        kvf_fixture_fill(b->expected, b->bytes * LAYERS / sizeof(float), 0);
        for (unsigned i = 0; i < b->clients; ++i)
            lanes[i] = (lane){.b = b, .id = i};
        ok = run_batch(b, lanes, WARMUP, &c);
    }
    for (unsigned round = 1; ok && round <= ROUNDS; ++round)
        ok = run_batch(b, lanes, SAMPLES, &c) && write_round(raw, summary, b, round, lanes, &c);
    mi_segment_pool_destroy(b->verify);
    free(b->expected);
    if (summary)
        ok = fclose(summary) == 0 && ok;
    if (raw)
        ok = fclose(raw) == 0 && ok;
    pthread_cond_destroy(&b->ready);
    pthread_mutex_destroy(&b->lock);
    return ok;
}

static int run_tcp(batch *b, counters c, const char *raw, const char *summary) {
    mi_remote_config cfg;
    mi_remote_config_defaults(&cfg);
    cfg.control = b->control.endpoint;
    cfg.lanes = b->clients;
    cfg.peer_connections = !strcmp(b->mode, "pooled") ? b->clients : 0;
    cfg.location = b->device < 0 ? MI_LOCATION_CPU : MI_LOCATION_GPU;
    cfg.timeout_ms = b->control.timeout_ms;
    snprintf(cfg.worker_name, sizeof(cfg.worker_name), "bench-%d", getpid());
    if (!kvf_fixture_identity(b->bytes, &cfg.identity) ||
        kvf_identity_hash(&cfg.identity, &b->key.space) != KVF_OK ||
        kvf_page_hash(&cfg.identity, (kvf_hash){0}, kvf_fixture_tokens, TOKENS_PER_PAGE, &b->key.page) != KVF_OK)
        return 0;
    mi_storage_config storage;
    mi_storage_config_default(&storage);
    storage.block_bytes = b->bytes;
    mi_storage_backends backends = {0};
    mi_status st = mi_mempool_create(b->device < 0 ? MI_MEMORY_CPU : MI_MEMORY_DEVICE,
                                     b->device, (mi_mempool_config){0}, &b->pool);
    if (st == MI_OK)
        st = mi_memory_backend_create(cfg.location, b->bytes, b->pool, b->clients * LAYERS, &b->backend);
    size_t budget = b->clients * LAYERS * (((size_t)b->bytes + 255) & ~(size_t)255);
    storage.cpu_bytes = b->device < 0 ? budget : 0;
    storage.gpu_bytes = b->device < 0 ? 0 : budget;
    if (b->device < 0)
        backends.cpu = b->backend;
    else
        backends.gpu = b->backend;
    if (st == MI_OK)
        st = mi_tiered_storage_create(storage, backends, &b->storage);
    kvf_status worker = st == MI_OK ? mi_remote_worker_start(b->storage, cfg, &b->worker) : mi_remote_from_storage(st);
    if (worker != KVF_OK)
        fprintf(stderr, "benchmark receiver: %s\n", kvf_status_string(worker));
    int ok = worker == KVF_OK && measure(b, c, raw, summary);
    mi_remote_worker_stop(b->worker);
    mi_tiered_storage_destroy(b->storage);
    mi_backend_destroy(b->backend);
    mi_mempool_destroy(b->pool);
    return ok;
}

static int run_ssd(batch *b, const char *dir, const char *raw, const char *summary) {
    struct statfs fs;
    if (statfs(dir, &fs) != 0 || fs.f_type != 0xef53) {
        fprintf(stderr, "SSD benchmark requires a Linux ext4 directory: %s\n", dir);
        return 0;
    }
    char file[1024];
    int n = snprintf(file, sizeof(file), "%s/records.XXXXXX", dir);
    if (n < 0 || (size_t)n >= sizeof(file))
        return 0;
    int fd = mkstemp(file);
    if (fd < 0)
        return 0;
    close(fd);
    size_t aligned = ((b->bytes + 4095) & ~(size_t)4095) + 4096;
    mi_status st = !strcmp(b->mode, "direct")
        ? mi_ssd_direct_backend_create(file, b->bytes, aligned * b->clients, MI_SSD_VERIFY_ON_READ,
                                       b->clients, -1, &b->backend)
        : mi_ssd_file_backend_create(file, b->bytes, aligned * b->clients, MI_SSD_VERIFY_ON_READ,
                                     MI_SSD_FILE_POSIX, 1, &b->backend);
    if (st != MI_OK)
        fprintf(stderr, "benchmark SSD: %s\n", mi_status_string(st));
    int ok = 0;
    if (st == MI_OK) {
        b->mode = mi_ssd_file_io_name(b->backend);
        fprintf(stderr, "SSD benchmark statfs=0x%lx mode=%s\n", (unsigned long)fs.f_type, b->mode);
        ok = measure(b, (counters){0}, raw, summary);
    }
    mi_backend_destroy(b->backend);
    unlink(file);
    return ok;
}

static uint32_t number(const char *value, unsigned max) {
    char *end;
    unsigned long n = strtoul(value, &end, 10);
    return *value && !*end && n && n <= max ? (uint32_t)n : 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && (!strcmp(argv[1], "fixture") || !strcmp(argv[1], "verify"))) {
        uint32_t bytes = number(argv[2], KVF_MAX_LAYER_BYTES);
        if (!bytes)
            return 2;
        if (!strcmp(argv[1], "fixture"))
            return !kvf_fixture_write(argv[3], bytes);
        kvf_hash sha;
        char hex[65];
        if (!kvf_fixture_check(bytes, argv[3], &sha)) {
            fputs("FP32 tensor or Attention verification failed\n", stderr);
            return 1;
        }
        kvf_hash_format(sha, hex);
        printf("SHA256 %s fp32_attention=byte-identical\n", hex);
        return 0;
    }
    if (argc == 11 && !strcmp(argv[1], "run")) {
        batch b = {.tcp = 1, .mode = argv[2], .device_name = argv[3],
                   .bytes = number(argv[5], KVF_MAX_LAYER_BYTES), .clients = number(argv[6], MAX_CLIENTS),
                   .control.timeout_ms = 12000};
        b.device = strcmp(argv[3], "cpu") ? 0 : -1;
        int valid_device = b.device < 0 || (!strcmp(argv[3], "cuda") && mi_device_backend() == MI_DEVICE_CUDA);
        counters c = {.pid[DIRECTORY] = (int)number(argv[7], INT32_MAX),
                      .pid[SOURCE] = (int)number(argv[8], INT32_MAX)};
        if (b.bytes && b.clients && c.pid[DIRECTORY] && c.pid[SOURCE] && valid_device &&
            (!strcmp(b.mode, "short") || !strcmp(b.mode, "pooled")) &&
            kvf_endpoint_parse(argv[4], &b.control.endpoint) == KVF_OK)
            return !run_tcp(&b, c, argv[9], argv[10]);
    }
    if (argc == 8 && !strcmp(argv[1], "ssd")) {
        batch b = {.mode = argv[2], .device_name = "cpu", .device = -1,
                   .bytes = number(argv[4], KVF_MAX_LAYER_BYTES), .clients = number(argv[5], MAX_CLIENTS)};
        if (b.bytes && !(b.bytes % sizeof(float)) && b.clients &&
            (!strcmp(b.mode, "buffered") || !strcmp(b.mode, "direct")))
            return !run_ssd(&b, argv[3], argv[6], argv[7]);
    }
    fputs("usage: kvf_bench fixture BYTES DIR | verify BYTES TENSORS\n"
          "       kvf_bench run short|pooled cpu|cuda CONTROL BYTES CLIENTS DIR_PID SOURCE_PID RAW SUMMARY\n"
          "       kvf_bench ssd buffered|direct EXT4_DIR BYTES CLIENTS RAW SUMMARY\n", stderr);
    return 2;
}
