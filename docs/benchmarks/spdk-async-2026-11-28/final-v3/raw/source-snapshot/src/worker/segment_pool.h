/*
 * 固定槽位的 Host Segment 池，供 Worker 传输和 SSD 定位 I/O 共用。
 * 池持有驱动分配，借用者持槽引用，必须在 DMA、socket 或磁盘完成后释放。
 * 无锁。release/acquire 保证旧借用者退出后新借用者才可访问。
 */
#ifndef MINIINFER_SEGMENT_POOL_H
#define MINIINFER_SEGMENT_POOL_H

#include "miniinfer/device_transfer.h"
#include <stdatomic.h>

enum { MI_SEGMENT_ALIGNMENT = 4096 };

typedef struct {
    atomic_uint ref_count;   /* 0 才允许借用或覆盖。 */
    atomic_uint quarantined; /* An unconfirmed DMA keeps this entire allocation. */
    unsigned char *data;     /* 借自 pool 的固定、对齐槽位。 */
    mi_stream stream;        /* 由池建立，供该槽唯一的 DMA 所有者复用。 */
    mi_event event;          /* 记录该槽最后一次异步传输，复用前必须同步。 */
} mi_segment;

typedef struct mi_segment_pool {
    void *allocation;        /* 原始驱动分配，释放时必须用这个地址。 */
    size_t stride;           /* 每个槽的总字节数，为 4 KiB 的倍数。 */
    unsigned count;          /* 创建后固定，关闭前须归还所有引用。 */
    int device;              /* -1 表示普通 CPU 主机内存。 */
    struct mi_segment_pool *retired_next;
    int retired;
    mi_segment slots[];      /* 与池同寿命，借用者不释放槽对象。 */
} mi_segment_pool;

mi_status mi_segment_pool_create(int device, size_t bytes, unsigned count, int streams, mi_segment_pool **out);
void mi_segment_pool_destroy(mi_segment_pool *pool);
mi_segment *mi_segment_acquire(mi_segment_pool *pool, unsigned first, unsigned end);
void mi_segment_release(mi_segment *segment);
void mi_segment_quarantine(mi_segment *segment);

#endif
