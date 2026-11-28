/* Explicit hardware validation. Never registered with ctest: writes only the
 * caller-supplied scratch region. Keep a durable backup before running it. */
#define _POSIX_C_SOURCE 200809L
#include "../src/worker/spdk_bridge.h"
#include "miniinfer/miniinfer.h"
#include "miniinfer/device_transfer.h"
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { BLOCK_BYTES = 512 * 1024, BLOCKS = 32, ROUNDS = 5 };
#define OK(expr) do { mi_status status_ = (expr); if (status_ != MI_OK) { \
    fprintf(stderr, "%s: %s\n", #expr, mi_status_string(status_)); exit(1); } } while (0)

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void pattern(unsigned char *p, size_t n, unsigned salt) {
    for (size_t i = 0; i < n; ++i)
        p[i] = (unsigned char)((i * 61u) ^ (i >> 8) ^ (salt * 17u));
}

static uint64_t number(const char *s) {
    char *end;
    errno = 0;
    unsigned long long n = strtoull(s, &end, 0);
    if (errno || !*s || *s == '-' || *end) { fputs("invalid number\n", stderr); exit(2); }
    return n;
}

static int raw(mi_spdk_config cfg, const char *mode, const char *backup) {
    size_t n = cfg.capacity_bytes;
    unsigned char *original = malloc(n), *actual = malloc(n), *expected = malloc(n);
    if (!original || !actual || !expected) return 1;
    FILE *file = fopen(backup, "rb");
    if (!file || fread(original, 1, n, file) != n || fgetc(file) != EOF) {
        fputs("backup size/read failure\n", stderr); return 1;
    }
    fclose(file);
    mi_spdk_device *device = NULL;
    OK(mi_spdk_open(cfg, &device));
    int failed = 0;
    if (!strcmp(mode, "raw")) {
        OK(mi_spdk_transfer(device, 0, (mi_io_range){.data=actual, .data_bytes=n}));
        if (memcmp(original, actual, n)) {
            fputs("scratch differs from backup; refusing raw test\n", stderr);
            mi_spdk_close(device); return 1;
        }
        pattern(expected, n, 1);
        double write_seconds = 0, read_seconds = 0;
        for (unsigned r = 0; r < ROUNDS; ++r) {
            pattern(expected, n, r + 1);
            double a = now();
            mi_status st = mi_spdk_transfer(device, 1,
                (mi_io_range){.data=expected, .data_bytes=n});
            double b = now();
            if (st != MI_OK) { failed=1; break; }
            st = mi_spdk_transfer(device, 0, (mi_io_range){.data=actual, .data_bytes=n});
            double c = now();
            if (st != MI_OK || memcmp(expected, actual, n)) { failed=1; break; }
            write_seconds += b-a;
            read_seconds += c-b;
        }
        /* Scatter/gather, unaligned RMW, and multiple 256-KiB DMA chunks.
         * Comparing the entire region also checks that adjacent bytes survive. */
        size_t offset = 19, header = 37, bytes = 3 * 256 * 1024 + 123;
        memset(expected + offset, 0xa7, header + bytes);
        mi_status st = mi_spdk_transfer(device, 1, (mi_io_range){
            .header=expected+offset, .header_bytes=header,
            .data=expected+offset+header, .data_bytes=bytes, .offset=offset});
        if (st != MI_OK) failed=1;
        st = mi_spdk_transfer(device, 0, (mi_io_range){.data=actual, .data_bytes=n});
        if (st != MI_OK || memcmp(expected, actual, n)) failed=1;
        if (!failed) printf("raw_payload_bytes=%zu rounds=%d write_gib_s=%.6f read_gib_s=%.6f unaligned_rmw=PASS\n",
            n, ROUNDS, (double)n*ROUNDS/(1ull<<30)/write_seconds,
            (double)n*ROUNDS/(1ull<<30)/read_seconds);
    }
    mi_status st = mi_spdk_transfer(device, 1, (mi_io_range){.data=original, .data_bytes=n});
    if (st != MI_OK) failed=1;
    st = mi_spdk_transfer(device, 0, (mi_io_range){.data=actual, .data_bytes=n});
    if (st != MI_OK || memcmp(original, actual, n)) failed=1;
    mi_spdk_close(device);
    free(expected); free(actual); free(original);
    printf("backup_restore=%s\n", failed ? "FAIL" : "PASS");
    return failed;
}

static int tiered(mi_spdk_config cfg, int cuda, unsigned rounds, unsigned warmup) {
    mi_mempool *cpu_pool = NULL, *gpu_pool = NULL, *pinned = NULL;
    mi_storage_backend *cpu = NULL, *gpu = NULL, *ssd = NULL;
    mi_tiered_storage *storage = NULL;
    mi_stream stream = NULL;
    unsigned char *expected = NULL, *actual = NULL;
    OK(mi_mempool_create(MI_MEMORY_CPU, -1, (mi_mempool_config){0}, &cpu_pool));
    OK(mi_memory_backend_create(MI_LOCATION_CPU, BLOCK_BYTES, cpu_pool, BLOCKS, &cpu));
    if (cuda) {
        OK(mi_mempool_create(MI_MEMORY_CUDA, 0, (mi_mempool_config){0}, &gpu_pool));
        OK(mi_mempool_create(MI_MEMORY_CUDA_PINNED, 0, (mi_mempool_config){0}, &pinned));
        OK(mi_memory_backend_create(MI_LOCATION_GPU, BLOCK_BYTES, gpu_pool, BLOCKS, &gpu));
        OK(mi_device_host_alloc(0, BLOCK_BYTES, (void **)&expected));
        OK(mi_device_host_alloc(0, BLOCK_BYTES, (void **)&actual));
        OK(mi_device_stream_create(0, &stream));
    } else {
        expected=malloc(BLOCK_BYTES); actual=malloc(BLOCK_BYTES);
        if (!expected || !actual) return 1;
    }
    OK(mi_ssd_spdk_backend_create(cfg, BLOCK_BYTES, MI_SSD_VERIFY_READ_BACK, &ssd));
    mi_storage_config config = {.block_bytes=BLOCK_BYTES,
        .cpu_bytes=(size_t)BLOCK_BYTES*BLOCKS, .gpu_bytes=cuda ? (size_t)BLOCK_BYTES*BLOCKS : 0,
        .ssd_bytes=cfg.capacity_bytes, .max_async_migrations=4, .retain_ssd_copy=0};
    OK(mi_tiered_storage_create(config, (mi_storage_backends){
        .cpu=cpu, .gpu=gpu, .ssd=ssd, .pinned_pool=pinned}, &storage));
    mi_location home = cuda ? MI_LOCATION_GPU : MI_LOCATION_CPU;
    mi_block_id ids[BLOCKS];
    for (unsigned i=0; i<BLOCKS; ++i) {
        pattern(expected, BLOCK_BYTES, i);
        OK(mi_storage_create_block(storage, BLOCK_BYTES, home, &ids[i]));
        mi_lease lease;
        OK(mi_storage_acquire(storage, ids[i], MI_ACCESS_WRITE, stream, &lease));
        if (cuda) OK(mi_device_copy_from_host(0, lease.data, expected, BLOCK_BYTES, stream));
        else memcpy(lease.data, expected, BLOCK_BYTES);
        OK(mi_lease_finish(&lease));
    }
    puts("mode,round,payload_bytes,spill_ms,recovery_ms,verified_bytes");
    for (unsigned r=0; r<warmup+rounds; ++r) {
        double a=now();
        for (unsigned i=0; i<BLOCKS; ++i) OK(mi_storage_migrate(storage, ids[i], MI_LOCATION_SSD));
        double b=now();
        for (unsigned i=0; i<BLOCKS; ++i) {
            mi_block_meta meta;
            OK(mi_storage_metadata(storage, ids[i], &meta));
            if (meta.location != MI_LOCATION_SSD || meta.version != 1) return 1;
        }
        double c=now();
        for (unsigned i=0; i<BLOCKS; ++i) OK(mi_storage_migrate(storage, ids[i], home));
        if (cuda) OK(mi_device_synchronize(0));
        double d=now();
        for (unsigned i=0; i<BLOCKS; ++i) {
            mi_block_meta meta;
            pattern(expected, BLOCK_BYTES, i);
            OK(mi_storage_metadata(storage, ids[i], &meta));
            if (meta.location != home || meta.version != 1) return 1;
            OK(mi_storage_read_snapshot(storage, ids[i], 1, actual, BLOCK_BYTES, stream, &meta));
            if (memcmp(actual, expected, BLOCK_BYTES)) { fputs("byte mismatch\n", stderr); return 1; }
        }
        if (r >= warmup)
            printf("%s,%u,%u,%.6f,%.6f,%u\n", cuda ? "cuda-spdk" : "cpu-spdk", r-warmup,
                BLOCK_BYTES*BLOCKS, (b-a)*1000, (d-c)*1000, BLOCK_BYTES*BLOCKS);
    }
    mi_tiered_storage_destroy(storage);
    mi_backend_destroy(ssd); mi_backend_destroy(gpu); mi_backend_destroy(cpu);
    mi_mempool_destroy(pinned); mi_mempool_destroy(gpu_pool); mi_mempool_destroy(cpu_pool);
    if (cuda) {
        OK(mi_device_stream_destroy(0, stream));
        OK(mi_device_host_free(0, actual)); OK(mi_device_host_free(0, expected));
    } else { free(actual); free(expected); }
    puts("tiered_crc_identity_and_bytes=PASS");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 6 || (strcmp(argv[5], "raw") && strcmp(argv[5], "restore") &&
        strcmp(argv[5], "cpu") && strcmp(argv[5], "cuda"))) {
        fprintf(stderr, "usage: %s CONFIG BDEV OFFSET CAPACITY raw|restore|cpu|cuda [BACKUP] [--rounds N --warmup N]\n", argv[0]);
        return 2;
    }
    uint64_t offset=number(argv[3]), capacity=number(argv[4]);
    if (offset%4096 || capacity%4096 || capacity<16*1024*1024 ||
        capacity>256*1024*1024 || offset>UINT64_MAX-capacity) return 2;
    cpu_set_t cpus;
    if (sched_getaffinity(0, sizeof(cpus), &cpus)) return 1;
    unsigned cpu=0;
    while (cpu<64 && !CPU_ISSET(cpu, &cpus)) ++cpu;
    if (cpu==64) return 1;
    char mask[32];
    snprintf(mask, sizeof(mask), "0x%llx", 1ull<<cpu);
    mi_spdk_config cfg={.json_config=argv[1], .bdev_name=argv[2], .reactor_mask=mask,
        .offset_bytes=offset, .capacity_bytes=(size_t)capacity,
        .iova_mode="va", .base_virtaddr=0x1000000000ull};
    if (!strcmp(argv[5], "raw") || !strcmp(argv[5], "restore")) {
        if (argc!=7) return 2;
        return raw(cfg, argv[5], argv[6]);
    }
    unsigned rounds=ROUNDS, warmup=0;
    for (int i=6; i<argc; i+=2) {
        if (i+1 >= argc) return 2;
        uint64_t value=number(argv[i+1]);
        if (!strcmp(argv[i], "--rounds") && value>0 && value<=1000)
            rounds=(unsigned)value;
        else if (!strcmp(argv[i], "--warmup") && value<=100)
            warmup=(unsigned)value;
        else return 2;
    }
    return tiered(cfg, !strcmp(argv[5], "cuda"), rounds, warmup);
}
