/*
 * 单机推理 KV 层。管理物理页、前缀索引和页引用。
 * 缓存持有 K/V Slab，活动请求借用页。由调度线程串行访问。
 */
#include "nano_cache.h"
#ifdef NANO_WITH_CUDA
#include "nano_cuda.h"
#endif
#ifdef NANO_WITH_STORAGE
#include "miniinfer/allocator.h"
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static size_t prefix_bucket(const NvPrefixIndex *index, NvPrefixKey key) {
    return (key.hash ^ (key.check >> 27) ^ (key.hash >> 32)) & index->mask;
}

/* 线性探测，前缀长度为 0 的桶可用于插入。 */
static size_t prefix_slot(const NvPrefixIndex *index, NvPrefixKey key) {
    size_t i = prefix_bucket(index, key);
    for (;;) {
        const NvPrefixKey *other = &index->entries[i].key;
        if (!other->length ||
            (other->length == key.length && other->hash == key.hash && other->check == key.check))
            return i;
        i = (i + 1) & index->mask;
    }
}

void nv_prefix_index_init(NvPrefixIndex *index, size_t capacity) {
    if (capacity > SIZE_MAX / sizeof(*index->entries) / 4)
        nv_fatal("prefix index size overflow");
    size_t buckets = 2;
    while (buckets / 2 < capacity)
        buckets *= 2;
    *index = (NvPrefixIndex){.entries = nv_alloc(buckets, sizeof(*index->entries)), .mask = buckets - 1};
}

int nv_prefix_find(const NvPrefixIndex *index, NvPrefixKey key) {
    const NvPrefixEntry *entry = &index->entries[prefix_slot(index, key)];
    return entry->key.length ? entry->id : -1;
}

void nv_prefix_insert(NvPrefixIndex *index, NvPrefixKey key, int id) {
    index->entries[prefix_slot(index, key)] = (NvPrefixEntry){key, id};
}

/* Move entries whose probe would otherwise stop at the deleted bucket. */
void nv_prefix_remove(NvPrefixIndex *index, NvPrefixKey key) {
    size_t gap = prefix_slot(index, key);
    size_t mask = index->mask;
    for (size_t i = (gap + 1) & mask; index->entries[i].key.length; i = (i + 1) & mask) {
        size_t home = prefix_bucket(index, index->entries[i].key);
        if (((gap - home) & mask) < ((i - home) & mask)) {
            index->entries[gap] = index->entries[i];
            gap = i;
        }
    }
    index->entries[gap] = (NvPrefixEntry){0};
}

size_t nv_cache_init_metadata(NvCache *c, const NvConfig *p, NvOptions o) {
    *c = (NvCache){.num_blocks = o.num_blocks, .block_size = o.block_size,
        .layers = p->n_layers, .kv_dim = p->dim / p->n_heads * p->n_kv_heads,
        .enabled = o.prefix_cache, .oldest = -1, .newest = -1};
    c->blocks = nv_alloc((size_t)c->num_blocks, sizeof(*c->blocks));
    c->free_ids = nv_alloc((size_t)c->num_blocks, sizeof(*c->free_ids));
    c->free_count = c->num_blocks;
    for (int i = 0; i < c->num_blocks; ++i)
        c->free_ids[i] = c->num_blocks - i - 1;
    if (c->enabled)
        nv_prefix_index_init(&c->prefixes, (size_t)c->num_blocks);

    size_t count = (size_t)c->num_blocks;
    int factors[] = {c->block_size, c->layers, c->kv_dim};
    for (size_t i = 0; i < sizeof(factors) / sizeof(factors[0]); ++i) {
        if (count > SIZE_MAX / (size_t)factors[i])
            nv_fatal("KV cache size overflow");
        count *= (size_t)factors[i];
    }
    if (count > SIZE_MAX / sizeof(float) / 2)
        nv_fatal("KV cache size overflow");
    c->elements = count;
    return count;
}

void nv_cache_init(NvCache *c, const NvConfig *p, NvOptions o) {
    size_t count = nv_cache_init_metadata(c, p, o);
#ifdef NANO_WITH_STORAGE
    if (o.pooled_buffers) {
        void *slab;
        mi_status st = mi_mempool_create(MI_MEMORY_CPU, -1, (mi_mempool_config){0}, &c->pool);
        if (st == MI_OK)
            st = mi_mempool_alloc_slab(c->pool, 2 * count * sizeof(float), 64, &slab);
        if (st != MI_OK)
            nv_fatal(mi_status_string(st));
        c->keys = slab;
    } else
#endif
        c->keys = nv_alloc(count * 2, sizeof(float));
    c->values = c->keys + count;
}

void nv_cache_free(NvCache *c) {
#ifdef NANO_WITH_STORAGE
    nv_spill_destroy(c->spill);
#endif
#ifdef NANO_WITH_CUDA
    nv_cuda_destroy(c->cuda);
#endif
#ifdef NANO_WITH_STORAGE
    if (c->pool)
        mi_mempool_destroy(c->pool);
    else
#endif
        free(c->keys);
    free(c->prefixes.entries);
    free(c->free_ids);
    free(c->blocks);
    *c = (NvCache){0};
}

float *nv_cache_key(NvCache *c, int block, int layer, int offset) {
    return c->keys + (((size_t)block * c->layers + layer) * c->block_size + offset) * c->kv_dim;
}

float *nv_cache_value(NvCache *c, int block, int layer, int offset) {
    return c->values + (((size_t)block * c->layers + layer) * c->block_size + offset) * c->kv_dim;
}

void nv_cache_copy_block(NvCache *c, int block, void *host, size_t half, int to_cache) {
#ifdef NANO_WITH_CUDA
    if (c->cuda) {
        nv_cuda_copy_block(c, block, host, half, to_cache);
        return;
    }
#endif
    void *kv[] = {nv_cache_key(c, block, 0, 0), nv_cache_value(c, block, 0, 0)};
    for (size_t i = 0; i < 2; ++i) {
        unsigned char *side = (unsigned char *)host + i * half;
        if (to_cache)
            memcpy(kv[i], side, half);
        else
            memcpy(side, kv[i], half);
    }
}

static void lru_remove(NvCache *c, int id) {
    NvBlock *b = &c->blocks[id];
    if (b->prev >= 0)
        c->blocks[b->prev].next = b->next;
    else
        c->oldest = b->next;
    if (b->next >= 0)
        c->blocks[b->next].prev = b->prev;
    else
        c->newest = b->prev;
}

/* 页重新被引用时，从可淘汰 LRU 摘下。 */
static void retain(NvCache *c, int id) {
    if (c->blocks[id].refs++ == 0)
        lru_remove(c, id);
}

/* 引用归零后，未发布页入空闲栈，已发布页保留在 LRU。 */
static void release(NvCache *c, int id) {
    NvBlock *b = &c->blocks[id];
    assert(b->refs > 0);
    if (--b->refs)
        return;
    if (!b->prefix.length) {
        c->free_ids[c->free_count++] = id;
        return;
    }
    b->prev = c->newest;
    b->next = -1;
    if (c->newest >= 0)
        c->blocks[c->newest].next = id;
    else
        c->oldest = id;
    c->newest = id;
}

/* 先取空闲页，耗尽时淘汰最早闲置的前缀页。 */
int nv_cache_allocate(NvCache *c) {
    int id = c->oldest;
    if (c->free_count) {
        id = c->free_ids[--c->free_count];
    } else if (id >= 0) {
#ifdef NANO_WITH_STORAGE
        if (c->spill)
            nv_spill_evict(c, id);
#endif
        nv_prefix_remove(&c->prefixes, c->blocks[id].prefix);
        lru_remove(c, id);
    } else {
        return -1;
    }
    c->blocks[id] = (NvBlock){.refs = 1};
    return id;
}

void nv_cache_release(NvCache *c, NvSequence *s) {
    for (int i = 0; i < s->num_blocks; ++i)
        release(c, s->block_table[i]);
    s->computed = s->num_blocks = 0;
}

/* 用本页 token 延长两种滚动指纹。 */
static NvPrefixKey extend_prefix(NvPrefixKey key, const int *tokens, int count) {
    if (!key.length) {
        key.hash = UINT64_C(14695981039346656037);
        key.check = UINT64_C(0x9e3779b97f4a7c15);
    }
    for (int i = 0; i < count; ++i) {
        uint32_t token = (uint32_t)tokens[i];
        key.hash = (key.hash ^ token) * UINT64_C(1099511628211);
        key.check ^= token + UINT64_C(0x9e3779b97f4a7c15) + (key.check << 6) + (key.check >> 2);
    }
    key.length += count;
    return key;
}

void nv_cache_reuse(NvCache *c, NvSequence *s) {
    if (!c->enabled)
        return;
    NvPrefixKey key = {0};
    /* 最后一个 token 仍需前向，为采样生成 logits。 */
    int limit = (s->num_tokens - 1) / c->block_size;
    for (int logical = 0; logical < limit; ++logical) {
        key = extend_prefix(key, s->tokens + logical * c->block_size, c->block_size);
        int id = nv_prefix_find(&c->prefixes, key);
        if (id >= 0) {
            retain(c, id);
        } else {
#ifdef NANO_WITH_STORAGE
            if (c->spill)
                id = nv_spill_restore(c, key);
#endif
            if (id < 0)
                break;
            c->blocks[id].prefix = key;
            nv_prefix_insert(&c->prefixes, key, id);
        }
        s->block_table[s->num_blocks++] = id;
        s->computed += c->block_size;
        c->hits += (uint64_t)c->block_size;
    }
}

/* 只发布计算完的完整页，供后续请求共享。 */
void nv_cache_publish(NvCache *c, NvSequence *s) {
    if (!c->enabled || !s->computed || s->computed % c->block_size)
        return;
    int logical = s->computed / c->block_size - 1;
    int id = s->block_table[logical];
    assert(c->blocks[id].refs == 1 && !c->blocks[id].prefix.length);
    NvPrefixKey parent = logical ? c->blocks[s->block_table[logical - 1]].prefix : (NvPrefixKey){0};
    NvPrefixKey key = extend_prefix(parent, s->tokens + logical * c->block_size, c->block_size);
    int existing = nv_prefix_find(&c->prefixes, key);
    /* 同一前缀已发布时，让当前序列引用已有页。 */
    if (existing >= 0) {
        retain(c, existing);
        release(c, id);
        s->block_table[logical] = existing;
    } else {
        c->blocks[id].prefix = key;
        nv_prefix_insert(&c->prefixes, key, id);
    }
}
