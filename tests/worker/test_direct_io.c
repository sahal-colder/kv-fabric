/*
 * Storage integration tests. The test owns its temporary ext4 file, tier objects
 * and device resources. Migration tickets are joined before corruption or teardown.
 */
#define _POSIX_C_SOURCE 200809L
#include "miniinfer/miniinfer.h"
#include "miniinfer/device_transfer.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { BYTES = 4096 + 73, DISK_STRIDE = 3 * 4096, MEMORY_STRIDE = 4352 };

static void write_block(mi_tiered_storage *s, mi_block_id id, int device, mi_stream stream,
                        const void *expected) {
    mi_lease lease;
    assert(mi_storage_acquire(s, id, MI_ACCESS_WRITE, stream, &lease) == MI_OK);
    assert(mi_device_copy_from_host(device, lease.data, expected, BYTES, stream) == MI_OK);
    assert(mi_lease_finish(&lease) == MI_OK);
}

int main(int argc, char **argv) {
    int device = argc == 2 && !strcmp(argv[1], "cuda") ? 0 : -1;
    char path[] = "/tmp/kvfabric-direct-test.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    mi_storage_backend *disk = NULL, *memory = NULL;
    mi_status opened = mi_ssd_direct_backend_create(path, BYTES, 2 * DISK_STRIDE,
                                                     MI_SSD_VERIFY_READ_BACK, 2, device, &disk);
    if (opened == MI_EUNSUPPORTED) {
        close(fd);
        unlink(path);
        return 77;
    }
    assert(opened == MI_OK);
    printf("observed file mode: %s\n", mi_ssd_file_io_name(disk));
    mi_mempool *pool;
    assert(mi_mempool_create(device < 0 ? MI_MEMORY_CPU : MI_MEMORY_DEVICE, device,
                             (mi_mempool_config){0}, &pool) == MI_OK);
    mi_location tier = device < 0 ? MI_LOCATION_CPU : MI_LOCATION_GPU;
    assert(mi_memory_backend_create(tier, BYTES, pool, 2, &memory) == MI_OK);
    mi_storage_config cfg;
    mi_storage_config_default(&cfg);
    cfg.block_bytes = BYTES;
    cfg.cpu_bytes = device < 0 ? 2 * MEMORY_STRIDE : 0;
    cfg.gpu_bytes = device < 0 ? 0 : 2 * MEMORY_STRIDE;
    cfg.ssd_bytes = 2 * DISK_STRIDE;
    mi_storage_backends backends = {.ssd = disk};
    if (device < 0)
        backends.cpu = memory;
    else
        backends.gpu = memory;
    mi_tiered_storage *s;
    assert(mi_tiered_storage_create(cfg, backends, &s) == MI_OK);
    void *raw;
    assert(mi_device_host_alloc(device, 4 * 4096, &raw) == MI_OK);
    unsigned char *expected = (unsigned char *)raw + ((-(uintptr_t)raw) & 63);
    unsigned char actual[BYTES];
    for (unsigned i = 0; i < BYTES; ++i)
        expected[i] = (unsigned char)(i * 61u);
    mi_stream stream;
    assert(mi_device_stream_create(device, &stream) == MI_OK);
    mi_block_id id;
    assert(mi_storage_create_block(s, BYTES, tier, &id) == MI_OK);
    write_block(s, id, device, stream, expected);
    mi_migration_ticket *ticket;
    assert(mi_storage_migrate_async(s, id, MI_LOCATION_SSD, &ticket) == MI_OK);
    assert(mi_migration_wait(ticket) == MI_OK);
    mi_migration_ticket_destroy(ticket);
    mi_block_meta meta;
    assert(mi_storage_read_snapshot(s, id, 1, actual, BYTES, NULL, &meta) == MI_OK);
    assert(meta.location == MI_LOCATION_SSD && !memcmp(actual, expected, BYTES));
    assert(mi_storage_migrate(s, id, tier) == MI_OK);
    assert(mi_storage_read_snapshot(s, id, 1, expected + 8192, BYTES, stream, &meta) == MI_OK);
    assert(!memcmp(expected, expected + 8192, BYTES));

    assert(mi_storage_migrate(s, id, MI_LOCATION_SSD) == MI_OK);
    unsigned char corrupt = expected[17] ^ 1;
    assert(pwrite(fd, &corrupt, 1, 4096 + 17) == 1);
    assert(mi_storage_read_snapshot(s, id, 1, actual, BYTES, NULL, &meta) == MI_ECRC);
    assert(mi_storage_migrate(s, id, tier) == MI_ECRC);
    assert(mi_storage_erase(s, id) == MI_OK);
    mi_block_id next;
    assert(mi_storage_create_block(s, BYTES, tier, &next) == MI_OK && (next >> 32) > (id >> 32));
    write_block(s, next, device, stream, expected);
    assert(mi_storage_migrate(s, next, MI_LOCATION_SSD) == MI_OK);
    assert(mi_storage_read_snapshot(s, next, 1, actual, BYTES, NULL, &meta) == MI_OK);
    assert(!memcmp(expected, actual, BYTES));

    /* A torn/truncated file cannot supply bytes under a valid in-memory version. */
    assert(ftruncate(fd, 4096) == 0);
    assert(mi_storage_read_snapshot(s, next, 1, actual, BYTES, NULL, &meta) == MI_EIO);
    assert(mi_storage_erase(s, next) == MI_OK);
    assert(mi_device_stream_destroy(device, stream) == MI_OK);
    assert(mi_device_host_free(device, raw) == MI_OK);
    mi_tiered_storage_destroy(s);
    mi_backend_destroy(memory);
    mi_backend_destroy(disk);
    mi_mempool_destroy(pool);
    assert(close(fd) == 0 && unlink(path) == 0);
    return 0;
}
