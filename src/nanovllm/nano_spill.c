/*
 * 单机推理 SSD 溢写层。维护可恢复的本地前缀和物理槽。
 * 溢写对象拥有中转内存和索引，借用后端。由调度线程串行访问。
 */
#define _POSIX_C_SOURCE 200809L
#include "nano_cache.h"
#include "../worker/io_metrics.h"
#include "miniinfer/storage_backend.h"
#include <stdlib.h>

typedef struct spill_slot {
    mi_storage_handle handle;
    uint64_t version;
    NvPrefixKey key;
} spill_slot;

/* 写回缓存管理 SSD 槽、前缀索引和双区中转缓冲。 */
struct NvSpill {
    mi_storage_backend *backend;
    mi_mempool *pool;
    NvPrefixIndex prefixes;
    spill_slot *slots;
    int count, next;
    size_t half, stride;
    unsigned char *staging;
    uint64_t reads, writes, version, restore_wait_ns;
};

void nv_cache_attach_ssd(NvCache *cache, mi_storage_backend *backend, int count) {
    size_t half = nv_cache_block_elements(cache) * sizeof(float);
    size_t payload = 2 * half;
    if (!cache->enabled || !backend || count <= 0 ||
        mi_backend_slot_bytes(backend) < payload || payload > SIZE_MAX - 4095)
        nv_fatal("invalid SSD prefix storage configuration");
    struct NvSpill *s = nv_alloc(1, sizeof(*s));
    s->backend = backend;
    s->count = count;
    s->half = half;
    s->stride = (payload + 4095) & ~(size_t)4095;
    if (s->stride > SIZE_MAX / 2)
        nv_fatal("SSD staging size overflow");
    s->slots = nv_alloc((size_t)count, sizeof(*s->slots));
    nv_prefix_index_init(&s->prefixes, (size_t)count);
    mi_memory_kind kind = cache->cuda ? MI_MEMORY_CUDA_PINNED : MI_MEMORY_CPU;
    mi_status st = mi_mempool_create(kind, cache->device, (mi_mempool_config){0}, &s->pool);
    void *staging = NULL;
    if (st == MI_OK)
        st = mi_mempool_alloc_slab(s->pool, 2 * s->stride, 4096, &staging);
    if (st != MI_OK)
        nv_fatal(mi_status_string(st));
    s->staging = staging;
    cache->spill = s;
}

void nv_spill_stats(const NvCache *cache, uint64_t *reads, uint64_t *writes) {
    const struct NvSpill *s = cache->spill;
    *reads = s ? s->reads : 0;
    *writes = s ? s->writes : 0;
}

uint64_t nv_spill_wait_ns(const NvCache *cache) {
    return cache->spill ? cache->spill->restore_wait_ns : 0;
}
static mi_block_meta record_meta(int index, uint64_t version, size_t bytes) {
    return (mi_block_meta){.id = (mi_block_id)index + 1, .location = MI_LOCATION_SSD,
                           .size = bytes, .version = version};
}

/* 将待淘汰的前缀页写入下一个 SSD 槽。 */
void nv_spill_evict(NvCache *cache, int block) {
    struct NvSpill *s = cache->spill;
    NvPrefixKey key = cache->blocks[block].prefix;
    if (nv_prefix_find(&s->prefixes, key) >= 0)
        return;

    /* SSD 槽按环形顺序替换。 */
    int index = s->next;
    spill_slot *slot = &s->slots[index];
    if (slot->handle) {
        if (mi_backend_release(s->backend, slot->handle) != MI_OK)
            return;
        nv_prefix_remove(&s->prefixes, slot->key);
        slot->handle = MI_INVALID_HANDLE;
    }
    mi_storage_handle handle;
    if (mi_backend_reserve(s->backend, 2 * s->half, &handle) != MI_OK)
        return;
    nv_cache_copy_block(cache, block, s->staging, s->half, 0);
    mi_block_meta meta = record_meta(index, ++s->version, 2 * s->half);
    if (mi_backend_write_record(s->backend, handle, &meta, s->staging) != MI_OK) {
        (void)mi_backend_release(s->backend, handle);
        return;
    }
    *slot = (spill_slot){.handle = handle, .version = meta.version, .key = key};
    nv_prefix_insert(&s->prefixes, key, index);
    s->next = (index + 1) % s->count;
    ++s->writes;
}

/* Allocation may spill a second block while the restored bytes occupy lane one. */
/* 校验 SSD 记录后，把该页装回推理缓存。 */
int nv_spill_restore(NvCache *cache, NvPrefixKey key) {
    struct NvSpill *s = cache->spill;
    int index = nv_prefix_find(&s->prefixes, key);
    if (index < 0)
        return -1;
    spill_slot *slot = &s->slots[index];
    uint64_t started=mi_io_now_ns();
    /* 恢复使用第二块中转区，第一块供分配期间可能触发的写回使用。 */
    unsigned char *data = s->staging + s->stride;
    mi_block_meta meta = record_meta(index, slot->version, 2 * s->half);
    if (mi_backend_read_validated(s->backend, slot->handle, &meta, data) != MI_OK) {
        s->restore_wait_ns += mi_io_now_ns()-started;
        return -1;
    }
    int block = nv_cache_allocate(cache);
    if (block >= 0) {
        nv_cache_copy_block(cache, block, data, s->half, 1);
        ++s->reads;
    }
    s->restore_wait_ns += mi_io_now_ns()-started;
    return block;
}

void nv_spill_destroy(struct NvSpill *s) {
    if (!s)
        return;
    mi_backend_destroy(s->backend);
    mi_mempool_destroy(s->pool);
    free(s->prefixes.entries);
    free(s->slots);
    free(s);
}
