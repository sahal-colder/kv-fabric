/*
 * 本地 KV Offload 层。管理冷热位置、迁移和预取。
 * 缓存持有条目和 Block ID，借用 Storage。先保护缓存元数据再获取块引用。
 */
#ifndef MINIINFER_KV_CACHE_H
#define MINIINFER_KV_CACHE_H

#include "tiered_storage.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mi_kv_cache_config {
    /* 调用方可使用的逻辑槽数。 */
    size_t logical_slots;
    /* 单槽的数据字节上限。 */
    size_t payload_bytes;
    /* 后台预取队列容量。 */
    size_t prefetch_queue_depth;
    /* 是否启动预取线程。 */
    int enable_prefetch_worker;
} mi_kv_cache_config;

struct mi_kv_entry;
struct mi_kv_job;

typedef struct mi_kv_cache {
    /* 借用的分层存储，须晚于缓存销毁。 */
    mi_tiered_storage *storage;
    /* 初始化时保存的容量与预取配置。 */
    mi_kv_cache_config config;
    /* 按逻辑槽号索引的块映射和 LRU 链。 */
    struct mi_kv_entry *entries;
    /* 后台预取任务的环形队列。 */
    struct mi_kv_job *queue;
    /* 前两项为 CPU 驻留 LRU 的首尾，后两项为队首和任务数。 */
    size_t oldest, newest, queue_head, queued;
    /* 保护槽表、LRU 和预取队列。 */
    pthread_mutex_t mutex;
    /* 分别通知新任务可取和预取结果可用。 */
    pthread_cond_t work, completed;
    /* 后台预取线程。 */
    pthread_t worker;
    /* 记录线程已启动及请求退出的状态。 */
    int worker_started, stopping;
} mi_kv_cache;

/* 创建固定逻辑槽表，可选启动后台预取。 */
mi_status mi_kv_cache_create(mi_tiered_storage *storage, mi_kv_cache_config config,
                             mi_kv_cache **out);
/* 覆盖指定逻辑槽，必要时将其他槽迁到 SSD。 */
mi_status mi_kv_cache_write(mi_kv_cache *cache, size_t slot, const void *data, size_t bytes);
/* 将指定槽迁回 CPU 后复制到 data，可选返回实际字节数。 */
mi_status mi_kv_cache_read(mi_kv_cache *cache, size_t slot, void *data, size_t capacity,
                           size_t *bytes_read);

/* 同步将指定槽准备到 CPU。 */
mi_status mi_kv_cache_prefetch(mi_kv_cache *cache, size_t slot);
/* 提交后台预取，重复的待处理任务合并。 */
mi_status mi_kv_cache_prefetch_async(mi_kv_cache *cache, size_t slot);
/* 等待该槽的预取结果，任务被替换时返回 MI_ESTATE。 */
mi_status mi_kv_cache_wait_prefetch(mi_kv_cache *cache, size_t slot);

/* 删除逻辑槽并回收底层块。 */
mi_status mi_kv_cache_erase(mi_kv_cache *cache, size_t slot);
/* 停止预取线程并释放缓存持有的块。 */
void mi_kv_cache_destroy(mi_kv_cache *cache);

#ifdef __cplusplus
}
#endif
#endif
