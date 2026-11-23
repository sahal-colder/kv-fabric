/*
 * MiniInfer 存储回归层，覆盖 tiered。
 * 夹具拥有临时存储和缓冲区。线程与租约在销毁前收尾。
 */
#include "miniinfer/miniinfer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* 验证 CPU 与 SSD 往返迁移、版本及保留副本。 */
int main(void) {
    mi_mempool *pool = NULL;
    mi_storage_backend *cpu = NULL, *ssd = NULL;
    mi_tiered_storage *s = NULL;
    mi_storage_config c;
    mi_storage_backends b;
    mi_block_id id;
    mi_lease l;
    mi_block_meta m;
    char src[4096], dst[4096];
    size_t n = 0;
    memset(src, 0x5a, sizeof(src));
    assert(mi_mempool_create(MI_MEMORY_CPU, -1, (mi_mempool_config){0}, &pool) == MI_OK);
    assert(mi_memory_backend_create(MI_LOCATION_CPU, 4096, pool, 4, &cpu) == MI_OK);
    assert(mi_ssd_backend_create(NULL, 4096, (4096 + 64) * 4, MI_SSD_VERIFY_ON_READ, &ssd) == MI_OK);
    mi_storage_config_default(&c);
    c.block_bytes = 4096;
    c.cpu_bytes = 4096;
    c.ssd_bytes = 4096 + 64;
    c.gpu_bytes = 0;
    c.retain_ssd_copy = 1;
    b = (mi_storage_backends){.cpu = cpu, .ssd = ssd};
    assert(mi_tiered_storage_create(c, b, &s) == MI_OK);

    assert(mi_storage_create_block(s, 4096, MI_LOCATION_CPU, &id) == MI_OK);
    assert(mi_storage_acquire(s, id, MI_ACCESS_WRITE, NULL, &l) == MI_OK);
    memcpy(l.data, src, sizeof(src));
    assert(mi_lease_finish(&l) == MI_OK);
    assert(mi_storage_migrate(s, id, MI_LOCATION_SSD) == MI_OK);
    assert(mi_storage_metadata(s, id, &m) == MI_OK && m.location == MI_LOCATION_SSD && m.version == 1);
    assert(mi_storage_read_to_host(s, id, dst, sizeof(dst), &n) == MI_OK && n == sizeof(dst));
    assert(memcmp(src, dst, sizeof(src)) == 0);
    {
        mi_migration_ticket *ticket = NULL;
        assert(mi_storage_migrate_async(s, id, MI_LOCATION_CPU, &ticket) == MI_OK);
        assert(mi_migration_wait(ticket) == MI_OK);
        mi_migration_ticket_destroy(ticket);
    }
    assert(mi_storage_acquire(s, id, MI_ACCESS_READ, NULL, &l) == MI_OK);
    assert(memcmp(l.data, src, sizeof(src)) == 0);
    assert(mi_lease_finish(&l) == MI_OK);

    assert(mi_storage_migrate(s, id, MI_LOCATION_SSD) == MI_ENOMEM);
    assert(mi_storage_acquire(s, id, MI_ACCESS_WRITE, NULL, &l) == MI_OK);
    memset(l.data, 0x33, l.size);
    assert(mi_lease_finish(&l) == MI_OK);
    assert(mi_storage_migrate(s, id, MI_LOCATION_SSD) == MI_OK);
    assert(mi_storage_erase(s, id) == MI_OK);
    mi_tiered_storage_destroy(s);
    mi_backend_destroy(ssd);
    mi_backend_destroy(cpu);
    mi_mempool_destroy(pool);
    puts("tiered ok");
    return 0;
}
