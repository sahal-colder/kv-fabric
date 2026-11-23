/*
 * 逻辑存储层的 Block、Lease 与迁移接口。
 * Storage 拥有块记录，借用后端。锁序为 Trie -> Storage -> Backend。
 */
#ifndef MINIINFER_TIERED_STORAGE_H
#define MINIINFER_TIERED_STORAGE_H
#include "storage_backend.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct mi_tiered_storage mi_tiered_storage;

typedef struct mi_storage_config {
    /* 逻辑块的最大数据字节数。 */
    size_t block_bytes;
    /* CPU 层预算，按物理槽步长换算成槽数。 */
    size_t cpu_bytes;
    /* GPU 层预算，按物理槽步长换算成槽数。 */
    size_t gpu_bytes;
    /* SSD 层预算，包含记录头与对齐开销。 */
    size_t ssd_bytes;
    /* 排队和执行中的异步迁移总数上限。 */
    size_t max_async_migrations;
    /* 块从 SSD 迁回内存后，保留 SSD 副本。 */
    int retain_ssd_copy;
} mi_storage_config;

/* 后端由调用方持有，分层存储存续期间独占使用。 */
typedef struct mi_storage_backends {
    /* 可选的 CPU 内存后端。 */
    mi_storage_backend *cpu;
    /* 可选的 CUDA 显存后端。 */
    mi_storage_backend *gpu;
    /* 可选的 SSD 后端。 */
    mi_storage_backend *ssd;
    /* 可选的锁页池，供 GPU 与 SSD 间的迁移中转。 */
    mi_mempool *pinned_pool;
} mi_storage_backends;

typedef struct mi_lease {
    /* 发放租约的分层存储。 */
    mi_tiered_storage *storage;
    /* 被借用的逻辑块。 */
    mi_block_id id;
    /* 读或写访问权限。 */
    mi_access_mode mode;
    /* 租约有效期间可用的数据地址。 */
    void *data;
    /* 可访问的有效数据字节数。 */
    size_t size;
    /* GPU 访问所用的流，CPU 租约为 NULL。 */
    mi_stream stream;
    /* 租约有效标志及关联的设备号。 */
    int active, device;
} mi_lease;

typedef struct mi_migration_ticket mi_migration_ticket;

/* 填入默认块大小与容量配置。 */
void mi_storage_config_default(mi_storage_config *config);
/* 按预算连接各层后端并建立逻辑块表。 */
mi_status mi_tiered_storage_create(mi_storage_config config,
                                   mi_storage_backends backends,
                                   mi_tiered_storage **out);

/* 返回逻辑块表的固定容量。 */
size_t mi_storage_block_capacity(const mi_tiered_storage *storage);
/* 在 CPU 或 GPU 创建未初始化的块，并返回逻辑 ID。 */
mi_status mi_storage_create_block(mi_tiered_storage *storage, size_t bytes,
                                  mi_location initial_location, mi_block_id *out);

/* 获取内存块租约。读要求已提交，写要求独占，GPU 访问须提供流。 */
mi_status mi_storage_acquire(mi_tiered_storage *storage, mi_block_id id,
                             mi_access_mode mode, mi_stream stream, mi_lease *out);
/* 结束租约。写入成功时提交数据并递增版本。 */
mi_status mi_lease_finish(mi_lease *lease);
/* 结束租约。取消原地写入会使块变为未初始化。 */
mi_status mi_lease_abort(mi_lease *lease);

/* 将块元数据复制到 out。 */
mi_status mi_storage_metadata(mi_tiered_storage *storage, mi_block_id id,
                              mi_block_meta *out);

/* 将块迁到目标层，保留逻辑 ID 和数据版本。 */
mi_status mi_storage_migrate(mi_tiered_storage *storage, mi_block_id id, mi_location target);

/* 顺序迁移各块，逐项写入 results，并返回首个错误。 */
mi_status mi_storage_migrate_batch(mi_tiered_storage *storage, const mi_block_id *ids,
                                   size_t count, mi_location target, mi_status *results);

/* 提交迁移并返回票据。队列满时返回 MI_EBUSY。 */
mi_status mi_storage_migrate_async(mi_tiered_storage *storage, mi_block_id id,
                                   mi_location target, mi_migration_ticket **out);
/* 等待票据完成并返回迁移结果。 */
mi_status mi_migration_wait(mi_migration_ticket *ticket);
/* 释放调用方持有的票据引用。 */
void mi_migration_ticket_destroy(mi_migration_ticket *ticket);

/* 读到主机缓冲区，按需校验 SSD 记录。 */
mi_status mi_storage_read_to_host(mi_tiered_storage *storage, mi_block_id id,
                                  void *dst, size_t capacity, size_t *bytes_read);
/* 在锁内校验版本并钉扎块，在锁外复制到主机。GPU 目标缓冲须为 CUDA pinned。 */
mi_status mi_storage_read_snapshot(mi_tiered_storage *storage, mi_block_id id, uint64_t version,
                                   void *dst, size_t capacity, mi_stream stream, mi_block_meta *out);
int mi_storage_gpu_device(const mi_tiered_storage *storage);

/* 重试释放待清理的物理槽。 */
mi_status mi_storage_retry_cleanup(mi_tiered_storage *storage, mi_block_id id);
/* 删除逻辑块并回收驻留槽及副本。 */
mi_status mi_storage_erase(mi_tiered_storage *storage, mi_block_id id);

/* 查询指定层当前是否还有空闲槽。 */
int mi_storage_has_room(mi_tiered_storage *storage, mi_location location);
/* 等待后台迁移结束，释放内部资源并解除后端绑定。 */
void mi_tiered_storage_destroy(mi_tiered_storage *storage);

#ifdef __cplusplus
}
#endif
#endif
