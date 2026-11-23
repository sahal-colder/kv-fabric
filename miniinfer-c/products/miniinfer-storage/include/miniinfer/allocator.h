/*
 * 存储分配层。池拥有普通、锁页和加速器分配，调用方借用地址。
 * 锁顺序：后端建立或销毁 Slab 时调用池，池锁不回调后端或 Storage。
 */
#ifndef MINIINFER_ALLOCATOR_H
#define MINIINFER_ALLOCATOR_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 内存由普通主机、POSIX 锁页、CUDA 设备或 CUDA 锁页分配器提供。 */
typedef enum mi_memory_kind {
    MI_MEMORY_CPU,
    MI_MEMORY_PINNED,
    MI_MEMORY_DEVICE,
    MI_MEMORY_DEVICE_PINNED,
    MI_MEMORY_CUDA = MI_MEMORY_DEVICE,
    MI_MEMORY_CUDA_PINNED = MI_MEMORY_DEVICE_PINNED
} mi_memory_kind;

typedef struct mi_mempool mi_mempool;

typedef struct mi_mempool_config {
    /* 空闲内存块的缓存上限，0 表示不缓存。 */
    size_t max_cached_bytes;
    /* 底层分配的字节预算，0 表示不限。 */
    size_t max_reserved_bytes;
    /* 池的最小对齐粒度，0 使用默认值。 */
    size_t alignment;
    /* 分桶用的大块内存的目标大小。 */
    size_t chunk_bytes;
} mi_mempool_config;

/* 创建指定类型的内存池。CUDA 类型需要有效的设备编号。 */
mi_status mi_mempool_create(mi_memory_kind kind, int device_id,
                            mi_mempool_config config, mi_mempool **out);
/* 返回内存池的分配类型。 */
mi_memory_kind mi_mempool_kind(const mi_mempool *pool);
/* 返回关联的 CUDA 设备号，普通主机池返回 -1。 */
int mi_mempool_device(const mi_mempool *pool);

/* 分配指定字节数并满足对齐要求，地址写入 out。 */
mi_status mi_mempool_alloc(mi_mempool *pool, size_t bytes, size_t alignment, void **out);
/* 分配独立的连续内存区，供固定槽或工作区使用。 */
mi_status mi_mempool_alloc_slab(mi_mempool *pool, size_t bytes, size_t alignment, void **out);
/* 归还内存。调用方应确保设备已停止使用该地址。 */
mi_status mi_mempool_release(mi_mempool *pool, void *ptr);
/* 在指定 CUDA 流记录事件，完成后才回收内存。 */
mi_status mi_mempool_release_async(mi_mempool *pool, void *ptr, mi_stream stream);
/* 检查待回收事件，归还已完成的块。 */
mi_status mi_mempool_collect(mi_mempool *pool);
/* 清理已空闲的底层分配。 */
mi_status mi_mempool_trim(mi_mempool *pool);
/* 释放内存池及其拥有的底层分配。 */
void mi_mempool_destroy(mi_mempool *pool);

#ifdef __cplusplus
}
#endif
#endif
