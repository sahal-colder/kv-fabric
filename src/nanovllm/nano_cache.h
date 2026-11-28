/*
 * 单机 KV 内部接口。描述本地页、前缀索引与可选溢写。
 * 缓存拥有页和索引。该层的状态由调度线程串行更新。
 */
#ifndef NANO_CACHE_H
#define NANO_CACHE_H

#include "nv.h"

typedef struct NvPrefixEntry {
    /* 完整页前缀键。 */
    NvPrefixKey key;
    /* 对应的内存页 ID 或 SSD 槽号。 */
    int id;
} NvPrefixEntry;

struct NvSpill;
struct mi_storage_backend;

/* 预分配前缀哈希表。 */
void nv_prefix_index_init(NvPrefixIndex *index, size_t capacity);
/* 查询前缀对应的页或槽，未命中返回 -1。 */
int nv_prefix_find(const NvPrefixIndex *index, NvPrefixKey key);
/* 将前缀键关联到页或槽。 */
void nv_prefix_insert(NvPrefixIndex *index, NvPrefixKey key, int id);
/* 删除前缀键并保持探测链可达。 */
void nv_prefix_remove(NvPrefixIndex *index, NvPrefixKey key);

/* 初始化页表和索引，返回单侧 K 或 V 的元素数。 */
size_t nv_cache_init_metadata(NvCache *cache, const NvConfig *config, NvOptions options);
/* K 和 V 每侧物理页的元素数。 */
static inline size_t nv_cache_block_elements(const NvCache *cache) {
    return cache->elements / (size_t)cache->num_blocks;
}

/* 在主机缓冲区与 KV 缓存之间复制整个物理页。 */
void nv_cache_copy_block(NvCache *cache, int block, void *host, size_t half, int to_cache);
#ifdef NANO_WITH_REMOTE_KV
/* 将已校验、已写入的物理页加入本地索引，再推进请求偏移。 */
void nv_cache_commit_remote(NvCache *cache, NvSequence *seq, int block);
void nv_cache_discard_remote(NvCache *cache, int block);
#endif

#ifdef NANO_WITH_STORAGE
/* 接入 SSD 后端。之后由缓存负责关闭后端。 */
void nv_cache_attach_ssd(NvCache *cache, struct mi_storage_backend *backend, int slots);
/* 查询成功写回和恢复的次数。 */
uint64_t nv_spill_wait_ns(const NvCache *cache);
void nv_spill_stats(const NvCache *cache, uint64_t *reads, uint64_t *writes);
/* 将待淘汰的前缀页写回 SSD。 */
void nv_spill_evict(NvCache *cache, int block);
/* 校验并恢复前缀页，成功返回页 ID。 */
int nv_spill_restore(NvCache *cache, NvPrefixKey key);
/* 关闭后端并释放 SSD 索引和中转区。 */
void nv_spill_destroy(struct NvSpill *spill);
#endif
#endif
