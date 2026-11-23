/*
 * Segment 内存层。单次分配包含所有连续槽，初始化时建立 DMA 资源。
 * 槽的原子引用为唯一所有权入口，分配、回收与元数据锁无关。
 */
#include "segment_pool.h"
#include <stdint.h>
#include <stdlib.h>

mi_status mi_segment_pool_create(int device, size_t bytes, unsigned count, int streams, mi_segment_pool **out) {
    const size_t mask = MI_SEGMENT_ALIGNMENT - 1;
    if (!bytes || !count || count > 512 || bytes > SIZE_MAX - mask)
        return MI_EINVAL;
    size_t stride = (bytes + mask) & ~mask;
    if (count > (SIZE_MAX - mask) / stride)
        return MI_EOVERFLOW;
    mi_segment_pool *pool = calloc(1, sizeof(*pool) + count * sizeof(*pool->slots));
    if (!pool)
        return MI_ENOMEM;
    mi_status st = mi_device_init(device);
    if (st != MI_OK) {
        free(pool);
        return st;
    }
    pool->device = device;
    pool->stride = stride;
    size_t total = stride * count;
    st = mi_device_host_alloc(device, total + (device < 0 ? 0 : mask), &pool->allocation);
    if (st != MI_OK)
        goto failed;
    pool->base = (unsigned char *)pool->allocation + ((-(uintptr_t)pool->allocation) & mask);
    for (unsigned i = 0; i < count; ++i) {
        mi_segment *slot = &pool->slots[i];
        atomic_init(&slot->ref_count, 0);
        slot->data = pool->base + i * stride;
        ++pool->count;
        if (streams && device >= 0) {
            st = mi_device_stream_create(device, &slot->stream);
            if (st == MI_OK)
                st = mi_device_event_create(device, &slot->event);
            if (st != MI_OK)
                goto failed;
        }
    }
    *out = pool;
    return MI_OK;

failed:
    mi_segment_pool_destroy(pool);
    return st;
}

void mi_segment_pool_destroy(mi_segment_pool *pool) {
    if (!pool)
        return;
    for (unsigned i = 0; i < pool->count; ++i) {
        (void)mi_device_event_destroy(pool->device, pool->slots[i].event);
        (void)mi_device_stream_destroy(pool->device, pool->slots[i].stream);
    }
    if (pool->allocation)
        (void)mi_device_host_free(pool->device, pool->allocation);
    (void)mi_device_cleanup(pool->device);
    free(pool);
}

mi_segment *mi_segment_acquire(mi_segment_pool *pool, unsigned first, unsigned end) {
    for (unsigned i = first; i < end; ++i) {
        unsigned free = 0;
        if (atomic_compare_exchange_strong_explicit(&pool->slots[i].ref_count, &free, 1,
                                                    memory_order_acquire, memory_order_relaxed))
            return &pool->slots[i];
    }
    return NULL;
}

void mi_segment_release(mi_segment *segment) {
    if (segment)
        atomic_store_explicit(&segment->ref_count, 0, memory_order_release);
}
