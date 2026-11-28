/* Explicit GPU -> Host -> SSD eviction and SSD -> GPU recovery.
 * File-owned, CRC enabled, sequential migrations; never a raw-device test.
 * All data checks are outside the timed recovery interval.
 */
#define _POSIX_C_SOURCE 200809L
#include "miniinfer/miniinfer.h"
#include "miniinfer/device_transfer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { BLOCKS = 144, BYTES = 512 * 1024, WARMUP = 3, SAMPLES = 15 };
#define OK(expr) do { mi_status st = (expr); if (st != MI_OK) { \
    fprintf(stderr, "%s: %s\n", #expr, mi_status_string(st)); exit(1); } } while (0)

static double now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) exit(1);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void location(mi_tiered_storage *storage, mi_block_id id, mi_location tier) {
    mi_block_meta meta;
    OK(mi_storage_metadata(storage, id, &meta));
    if (meta.location != tier) { fputs("incorrect source/resident tier\n", stderr); exit(1); }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s PRIVATE_EXT4_DIR EXPECTED_IO_MODE\n", argv[0]);
        return 2;
    }
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/recovery.XXXXXX", argv[1]);
    if (n < 0 || (size_t)n >= sizeof(path)) return 2;
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 1; }
    close(fd);
    mi_mempool *cpu_pool, *gpu_pool, *pinned_pool;
    mi_storage_backend *cpu, *gpu, *ssd;
    OK(mi_mempool_create(MI_MEMORY_CPU, -1, (mi_mempool_config){0}, &cpu_pool));
    OK(mi_mempool_create(MI_MEMORY_CUDA, 0, (mi_mempool_config){0}, &gpu_pool));
    OK(mi_mempool_create(MI_MEMORY_CUDA_PINNED, 0, (mi_mempool_config){0}, &pinned_pool));
    OK(mi_memory_backend_create(MI_LOCATION_CPU, BYTES, cpu_pool, BLOCKS, &cpu));
    OK(mi_memory_backend_create(MI_LOCATION_GPU, BYTES, gpu_pool, BLOCKS, &gpu));
    OK(mi_ssd_direct_backend_create(path, BYTES, (BYTES + 4096ull) * BLOCKS,
                                   MI_SSD_VERIFY_ON_READ, 4, 0, &ssd));
    const char *mode = mi_ssd_file_io_name(ssd);
    if (strcmp(mode, argv[2])) { fprintf(stderr, "expected %s, observed %s\n", argv[2], mode); return 1; }
    mi_storage_config cfg = {.block_bytes = BYTES, .cpu_bytes = (size_t)BYTES * BLOCKS,
        .gpu_bytes = (size_t)BYTES * BLOCKS, .ssd_bytes = (BYTES + 4096ull) * BLOCKS,
        .max_async_migrations = 4, .retain_ssd_copy = 0};
    mi_tiered_storage *storage;
    OK(mi_tiered_storage_create(cfg, (mi_storage_backends){.cpu=cpu, .gpu=gpu,
                               .ssd=ssd, .pinned_pool=pinned_pool}, &storage));
    unsigned char *expected, *actual;
    OK(mi_device_host_alloc(0, BYTES, (void **)&expected));
    OK(mi_device_host_alloc(0, BYTES, (void **)&actual));
    mi_stream stream;
    OK(mi_device_stream_create(0, &stream));
    mi_block_id ids[BLOCKS];
    for (unsigned i = 0; i < BLOCKS; ++i) {
        for (size_t j = 0; j < BYTES; ++j) expected[j] = (unsigned char)(j * 61u + i * 17u);
        OK(mi_storage_create_block(storage, BYTES, MI_LOCATION_GPU, &ids[i]));
        mi_lease lease;
        OK(mi_storage_acquire(storage, ids[i], MI_ACCESS_WRITE, stream, &lease));
        OK(mi_device_copy_from_host(0, lease.data, expected, BYTES, stream));
        OK(mi_lease_finish(&lease));
    }
    OK(mi_device_stream_wait(0, stream));
    puts("mode,sample,payload_bytes,blocks,gpu_to_host_ms,host_to_ssd_ms,ssd_to_gpu_ms,effective_recovery_gib_s,verified_bytes");
    for (int sample = -WARMUP; sample < SAMPLES; ++sample) {
        double a = now();
        for (unsigned i = 0; i < BLOCKS; ++i) OK(mi_storage_migrate(storage, ids[i], MI_LOCATION_CPU));
        double b = now();
        for (unsigned i = 0; i < BLOCKS; ++i) location(storage, ids[i], MI_LOCATION_CPU);
        double c = now();
        for (unsigned i = 0; i < BLOCKS; ++i) OK(mi_storage_migrate(storage, ids[i], MI_LOCATION_SSD));
        double d = now();
        for (unsigned i = 0; i < BLOCKS; ++i) location(storage, ids[i], MI_LOCATION_SSD);
        double e = now();
        for (unsigned i = 0; i < BLOCKS; ++i) OK(mi_storage_migrate(storage, ids[i], MI_LOCATION_GPU));
        OK(mi_device_synchronize(0));
        double f = now();
        for (unsigned i = 0; i < BLOCKS; ++i) {
            location(storage, ids[i], MI_LOCATION_GPU);
            for (size_t j = 0; j < BYTES; ++j) expected[j] = (unsigned char)(j * 61u + i * 17u);
            mi_block_meta meta;
            OK(mi_storage_read_snapshot(storage, ids[i], 1, actual, BYTES, stream, &meta));
            if (memcmp(actual, expected, BYTES)) { fputs("byte mismatch\n", stderr); return 1; }
        }
        if (sample >= 0) printf("%s,%d,%zu,%d,%.6f,%.6f,%.6f,%.6f,%zu\n", mode, sample,
            (size_t)BLOCKS * BYTES, BLOCKS, (b-a)*1000, (d-c)*1000, (f-e)*1000,
            ((double)BLOCKS * BYTES / (1ull<<30)) / (f-e), (size_t)BLOCKS * BYTES);
        fflush(stdout);
    }
    OK(mi_device_stream_destroy(0, stream));
    OK(mi_device_host_free(0, actual));
    OK(mi_device_host_free(0, expected));
    mi_tiered_storage_destroy(storage);
    mi_backend_destroy(ssd); mi_backend_destroy(gpu); mi_backend_destroy(cpu);
    mi_mempool_destroy(pinned_pool); mi_mempool_destroy(gpu_pool); mi_mempool_destroy(cpu_pool);
    unlink(path);
    return 0;
}
