/*
 * 示例入口。演示分配、WRITE Lease 与 CPU 到文件层的迁移。
 * main 拥有池、Storage 和后端，并在归还 Lease 后逆序销毁。
 */
#include "miniinfer/miniinfer.h"
#include <stdio.h>
#include <string.h>

/* 示例展示分配 CPU 块、提交写租约和迁移到 SSD 的完整路径。 */
int main(void) {
    mi_mempool *pool = NULL;
    mi_storage_backend *cpu = NULL, *ssd = NULL;
    mi_tiered_storage *storage = NULL;
    mi_storage_config cfg;
    mi_storage_backends backends;
    mi_block_id id = MI_INVALID_BLOCK_ID;
    mi_lease lease;
    unsigned char data[4096];
    mi_status st;

    memset(data, 0x5a, sizeof(data));

    mi_mempool_config pool_config = {
        .chunk_bytes = 4 * sizeof(data),
        .max_cached_bytes = 16 * sizeof(data),
        .max_reserved_bytes = 16 * sizeof(data),
        .alignment = 256,
    };
    if ((st = mi_mempool_create(MI_MEMORY_CPU, -1, pool_config, &pool)) != MI_OK)
        goto fail;
    if ((st = mi_memory_backend_create(MI_LOCATION_CPU, sizeof(data),
                                       pool, 16, &cpu)) != MI_OK)
        goto fail;
    if ((st = mi_ssd_backend_create(NULL, sizeof(data),
                                    (sizeof(data) + 64) * 16,
                                    MI_SSD_VERIFY_ON_READ, &ssd)) != MI_OK)
        goto fail;

    mi_storage_config_default(&cfg);
    cfg.block_bytes = sizeof(data);

    cfg.cpu_bytes = sizeof(data) * 4;
    cfg.ssd_bytes = (sizeof(data) + 64) * 16;
    backends = (mi_storage_backends){.cpu = cpu, .gpu = NULL, .ssd = ssd};
    if ((st = mi_tiered_storage_create(cfg, backends, &storage)) != MI_OK)
        goto fail;
    /* 为数据预留一个 CPU 槽，并取得迁移期间稳定的逻辑 ID。 */
    if ((st = mi_storage_create_block(storage, sizeof(data), MI_LOCATION_CPU, &id)) != MI_OK)
        goto fail;
    /* 通过独占写租约取得 CPU 槽地址。 */
    if ((st = mi_storage_acquire(storage, id, MI_ACCESS_WRITE, NULL, &lease)) != MI_OK)
        goto fail;

    memcpy(lease.data, data, sizeof(data));
    /* 提交写入的数据，之后才能迁往 SSD。 */
    if ((st = mi_lease_finish(&lease)) != MI_OK)
        goto fail;
    /* 迁移保留逻辑 ID，改变实际驻留位置。 */
    if ((st = mi_storage_migrate(storage, id, MI_LOCATION_SSD)) != MI_OK)
        goto fail;
    printf("block %llu is now on SSD (version %llu)\n",
           (unsigned long long)id, (unsigned long long)1);

    (void)mi_storage_erase(storage, id);
    mi_tiered_storage_destroy(storage);
    mi_backend_destroy(ssd);
    mi_backend_destroy(cpu);
    mi_mempool_destroy(pool);
    return 0;

fail:
    fprintf(stderr, "MiniInfer C storage failed: %s\n", mi_status_string(st));
    if (storage)
        mi_tiered_storage_destroy(storage);
    if (ssd)
        mi_backend_destroy(ssd);
    if (cpu)
        mi_backend_destroy(cpu);
    mi_mempool_destroy(pool);
    return 1;
}
