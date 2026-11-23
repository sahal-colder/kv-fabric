/*
 * Logical page and Prefix Trie layer. The cache owns pages and block IDs.
 * Sequence, index, and lease references keep pages alive. Lock order is Trie -> Storage -> Backend.
 */
#ifndef MINIINFER_PAGED_KV_H
#define MINIINFER_PAGED_KV_H

#include "tiered_storage.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mi_kv_page;

typedef struct mi_paged_kv {
    /* 借用的分层存储，负责页内各层的数据块。 */
    mi_tiered_storage *storage;
    /* 记录页形状、总页数和当前空闲页数。 */
    size_t layers, tokens_per_page, bytes_per_layer, max_pages, free_count;
    /* 空闲页 ID 栈与开放寻址的前缀表。 */
    uint32_t *free_ids, *prefix_buckets;
    /* 哈希桶掩码。 */
    size_t prefix_mask;
    /* 互斥锁内推进的访问序号，用于挑选最久未使用的叶子。 */
    uint64_t prefix_clock;
    /* 按物理页 ID 索引的引用计数和前缀元数据。 */
    struct mi_kv_page *pages;
    /* 按页 ID、模型层排列的逻辑块 ID。 */
    mi_block_id *blocks;
    /* 按页 ID 存放的已发布 token。 */
    int32_t *tokens;
    /* 保护页表、引用计数和前缀索引。 */
    pthread_mutex_t mutex;
} mi_paged_kv;

typedef struct mi_kv_sequence {
    /* 借用的页池，由序列或租约持有页引用。 */
    mi_paged_kv *kv;
    /* 序列当前持有的页数。 */
    size_t count;
    /* 按序列顺序排列的物理页 ID。 */
    uint32_t *pages;
} mi_kv_sequence;

typedef struct mi_page_lease {
    /* 底层逻辑块的读写租约。 */
    mi_lease lease;
    /* 借用的页池，由序列或租约持有页引用。 */
    mi_paged_kv *kv;
    /* 租约持有引用的物理页 ID。 */
    uint32_t page;
} mi_page_lease;

/* 封存页的独立引用，归还前该页的逻辑 Block ID 保持有效。 */
typedef struct mi_page_ref {
    mi_paged_kv *kv; /* 拥有独立页面引用的池，NULL 表示已归还。 */
    uint32_t page;   /* 借用期间禁止回收或复用的物理页索引。 */
} mi_page_ref;

/* 按层数与页大小建立固定容量页池。storage 由调用方管理。 */
mi_status mi_paged_kv_create(mi_tiered_storage *storage, size_t layers,
                             size_t tokens_per_page, size_t bytes_per_layer,
                             const char *name_space, mi_paged_kv **out);
/* 创建空序列，使用指定页池。 */
mi_status mi_kv_sequence_create(mi_paged_kv *kv, mi_kv_sequence **out);

/* 在指定内存层为序列追加一页，并为每层分配块。 */
mi_status mi_kv_sequence_append_page(mi_kv_sequence *sequence, mi_location location);
/* 查询序列当前的逻辑页数。 */
size_t mi_kv_sequence_page_count(const mi_kv_sequence *sequence);
mi_status mi_kv_sequence_truncate(mi_kv_sequence *sequence, size_t keep_pages);
mi_status mi_kv_sequence_retain_page(const mi_kv_sequence *sequence, size_t page, mi_page_ref *out);
mi_status mi_page_ref_clone(const mi_page_ref *ref, mi_page_ref *out);
mi_block_id mi_page_ref_block(const mi_page_ref *ref, size_t layer);
void mi_page_ref_release(mi_page_ref *ref);
/* Workers use this pair to choose an idle export, retire its Directory replica,
 * then re-check local references before unlinking it. On success ref is consumed. */
int mi_page_ref_idle_leaf(const mi_page_ref *ref, uint64_t *last_used);
int mi_page_ref_evict_leaf(mi_page_ref *ref);
/* 获取序列指定页、指定层的块租约。已发布页只允许读。 */
mi_status mi_paged_kv_acquire(mi_kv_sequence *sequence, size_t page, size_t layer,
                              mi_access_mode mode, mi_stream stream, mi_page_lease *out);
/* 完成底层租约并归还页引用。 */
mi_status mi_page_lease_finish(mi_page_lease *lease);
/* 放弃写入，底层租约结束后归还页引用。 */
mi_status mi_page_lease_abort(mi_page_lease *lease);

/* Seal a page after all layer writes commit, without adding a prefix index reference. */
mi_status mi_kv_sequence_seal_page(mi_kv_sequence *sequence, size_t page, const int32_t *tokens);
/* 将序列开头的完整页封存并加入前缀索引。 */
mi_status mi_kv_sequence_publish(mi_kv_sequence *sequence, const int32_t *tokens,
                                 size_t token_count, size_t complete_pages);
/* 匹配最长完整页前缀并返回持有命中页引用的新序列。 */
mi_status mi_paged_kv_match(mi_paged_kv *kv, const int32_t *tokens, size_t token_count,
                            mi_kv_sequence **out, size_t *matched_tokens);

/* 清空前缀索引并归还索引持有的引用。 */
mi_status mi_paged_kv_evict_prefixes(mi_paged_kv *kv);
/* 释放序列持有的页引用。 */
void mi_kv_sequence_destroy(mi_kv_sequence *sequence);
/* 销毁页池。调用前须先结束租约并销毁序列。 */
void mi_paged_kv_destroy(mi_paged_kv *kv);

#ifdef __cplusplus
}
#endif
#endif
