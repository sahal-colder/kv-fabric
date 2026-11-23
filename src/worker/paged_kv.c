/*
 * Logical pages and Prefix Trie. Sequence/index/leases each hold a ref under the Trie lock.
 * The last ref erases the per-layer blocks. Lock order is Trie -> Storage -> Backend.
 */
#include "miniinfer/paged_kv.h"
#include "tiered_internal.h"
#include <stdlib.h>
#include <string.h>

/* 页的序列、前缀索引和租约各自持有引用。 */
typedef struct mi_kv_page {
    size_t refs;         /* 0 only for a free slot. A linked edge contributes one ref. */
    uint64_t edge_hash;  /* Hash of parent slot plus full page tokens. */
    uint64_t last_used;  /* Logical access time, advanced under the Trie lock. */
    uint32_t parent;     /* One-based parent ID, or 0 for a root edge. */
    uint32_t children;   /* Number of indexed children, so only leaves can be reclaimed. */
    unsigned sealed;     /* Immutable after every layer completes a write. */
    unsigned linked;     /* Exactly one occupied bucket owns the index ref when set. */
} mi_kv_page;

/* 页号乘层数，得到该页在连续块表中的起点。 */
static mi_block_id *page_blocks(const mi_paged_kv *k, uint32_t page) {
    return k->blocks + (size_t)page * k->layers;
}

/* 页号乘每页 token 数，定位该页的前缀数据。 */
static int32_t *page_tokens(const mi_paged_kv *k, uint32_t page) {
    return k->tokens + (size_t)page * k->tokens_per_page;
}

static void page_release(mi_paged_kv *k, uint32_t id) {
    mi_kv_page *page = &k->pages[id];
    if (--page->refs)
        return;
    mi_block_id *blocks = page_blocks(k, id);
    for (size_t i = 0; i < k->layers; ++i)
        (void)mi_storage_erase(k->storage, blocks[i]);
    *page = (mi_kv_page){0};
    k->free_ids[k->free_count++] = id;
}

static uint64_t edge_hash(uint32_t parent, const int32_t *tokens, size_t count) {
    uint64_t hash = UINT64_C(14695981039346656037) ^ parent;
    for (size_t i = 0; i < count; ++i) {
        hash ^= (uint32_t)tokens[i];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= hash >> 32;
    return hash;
}

/* Hash the parent and page tokens. Compare full edges on hash hits. */
static size_t prefix_slot(const mi_paged_kv *k, uint32_t parent, uint64_t hash, const int32_t *tokens) {
    size_t i = hash & k->prefix_mask;
    while (k->prefix_buckets[i]) {
        uint32_t id = k->prefix_buckets[i] - 1;
        mi_kv_page *p = &k->pages[id];
        if (p->edge_hash == hash && p->parent == parent &&
            !memcmp(page_tokens(k, id), tokens, k->tokens_per_page * sizeof(*tokens)))
            break;
        i = (i + 1) & k->prefix_mask;
    }
    return i;
}

static void unlink_leaf(mi_paged_kv *k, uint32_t id) {
    mi_kv_page *p = &k->pages[id];
    size_t i = prefix_slot(k, p->parent, p->edge_hash, page_tokens(k, id));
    k->prefix_buckets[i] = 0;
    /* Entries behind the hole must be reinserted so probing still reaches them. */
    for (i = (i + 1) & k->prefix_mask; k->prefix_buckets[i]; i = (i + 1) & k->prefix_mask) {
        uint32_t next = k->prefix_buckets[i] - 1;
        mi_kv_page *moved = &k->pages[next];
        k->prefix_buckets[i] = 0;
        k->prefix_buckets[prefix_slot(k, moved->parent, moved->edge_hash, page_tokens(k, next))] = next + 1;
    }
    if (p->parent)
        --k->pages[p->parent - 1].children;
    p->linked = 0;
    page_release(k, id);
}

static int idle_leaf(mi_paged_kv *k, uint32_t id, size_t expected_refs) {
    const mi_kv_page *p = &k->pages[id];
    return p->linked && !p->children && p->refs == expected_refs &&
        mi_storage_blocks_ready(k->storage, page_blocks(k, id), k->layers) == MI_OK;
}

static int reclaim_local_leaf(mi_paged_kv *k) {
    size_t candidate = k->max_pages;
    for (size_t i = 0; i < k->max_pages; ++i) {
        if ((candidate == k->max_pages || k->pages[i].last_used < k->pages[candidate].last_used) &&
            idle_leaf(k, (uint32_t)i, 1))
            candidate = i;
    }
    if (candidate == k->max_pages)
        return 0;
    unlink_leaf(k, (uint32_t)candidate);
    return 1;
}

mi_status mi_paged_kv_create(mi_tiered_storage *storage, size_t layers, size_t tokens_per_page,
                             size_t bytes_per_layer, const char *name_space, mi_paged_kv **out) {
    if (!storage || !layers || !tokens_per_page || !bytes_per_layer || !name_space || !*name_space || !out)
        return MI_EINVAL;
    size_t blocks = mi_storage_block_capacity(storage);
    size_t pages = blocks / layers;
    if (!pages)
        return MI_ENOMEM;
    if (pages > SIZE_MAX / sizeof(uint32_t) / 4 ||
        tokens_per_page > SIZE_MAX / sizeof(int32_t) / pages)
        return MI_EOVERFLOW;
    size_t buckets = 2;
    while (buckets / 2 < pages)
        buckets *= 2;

    mi_paged_kv *k = calloc(1, sizeof(*k));
    if (!k)
        return MI_ENOMEM;
    k->pages = calloc(pages, sizeof(*k->pages));
    k->free_ids = calloc(pages, sizeof(*k->free_ids));
    k->blocks = calloc(pages * layers, sizeof(*k->blocks));
    k->tokens = calloc(pages * tokens_per_page, sizeof(*k->tokens));
    k->prefix_buckets = calloc(buckets, sizeof(*k->prefix_buckets));
    mi_status st = MI_ENOMEM;
    if (!k->pages || !k->free_ids || !k->blocks || !k->tokens || !k->prefix_buckets)
        goto failed;
    st = MI_EINTERNAL;
    if (pthread_mutex_init(&k->mutex, NULL))
        goto failed;

    k->storage = storage;
    k->layers = layers;
    k->tokens_per_page = tokens_per_page;
    k->bytes_per_layer = bytes_per_layer;
    k->prefix_mask = buckets - 1;
    k->max_pages = k->free_count = pages;
    for (size_t i = 0; i < pages; ++i)
        k->free_ids[i] = (uint32_t)(pages - i - 1);
    *out = k;
    return MI_OK;

failed:
    free(k->prefix_buckets);
    free(k->tokens);
    free(k->blocks);
    free(k->free_ids);
    free(k->pages);
    free(k);
    return st;
}

mi_status mi_kv_sequence_create(mi_paged_kv *k, mi_kv_sequence **out) {
    if (!k || !out)
        return MI_EINVAL;
    mi_kv_sequence *s = malloc(sizeof(*s) + k->max_pages * sizeof(*s->pages));
    if (!s)
        return MI_ENOMEM;
    *s = (mi_kv_sequence){.kv = k, .pages = (uint32_t *)(s + 1)};
    *out = s;
    return MI_OK;
}

mi_status mi_kv_sequence_append_page(mi_kv_sequence *s, mi_location loc) {
    if (!s || (unsigned)loc > MI_LOCATION_GPU)
        return MI_EINVAL;
    mi_paged_kv *k = s->kv;
    pthread_mutex_lock(&k->mutex);
    mi_status st = MI_ENOMEM;
    while (s->count < k->max_pages) {
        if (k->free_count) {
            /* A failed allocation never consumes a page ID or publishes any layer. */
            uint32_t id = k->free_ids[k->free_count - 1];
            mi_block_id *blocks = page_blocks(k, id);
            size_t layer = 0;
            for (; layer < k->layers; ++layer) {
                st = mi_storage_create_block(k->storage, k->bytes_per_layer, loc, &blocks[layer]);
                if (st != MI_OK)
                    break;
            }
            if (st == MI_OK) {
                --k->free_count;
                k->pages[id].refs = 1;
                s->pages[s->count++] = id;
                break;
            }
            while (layer)
                (void)mi_storage_erase(k->storage, blocks[--layer]);
        }
        if (st != MI_ENOMEM || !reclaim_local_leaf(k))
            break;
    }
    pthread_mutex_unlock(&k->mutex);
    return st;
}

size_t mi_kv_sequence_page_count(const mi_kv_sequence *s) {
    if (!s)
        return 0;
    pthread_mutex_lock(&s->kv->mutex);
    size_t count = s->count;
    pthread_mutex_unlock(&s->kv->mutex);
    return count;
}

mi_status mi_kv_sequence_truncate(mi_kv_sequence *s, size_t keep_pages) {
    if (!s)
        return MI_EINVAL;
    mi_paged_kv *k = s->kv;
    pthread_mutex_lock(&k->mutex);
    mi_status st = keep_pages <= s->count ? MI_OK : MI_EINVAL;
    if (st == MI_OK)
        while (s->count > keep_pages)
            page_release(k, s->pages[--s->count]);
    pthread_mutex_unlock(&k->mutex);
    return st;
}

static mi_status retain_sealed(mi_paged_kv *k, uint32_t id, mi_page_ref *out) {
    mi_kv_page *page = &k->pages[id];
    if (!page->sealed || !page->refs)
        return MI_ESTATE;
    if (page->refs == SIZE_MAX)
        return MI_EOVERFLOW;
    ++page->refs;
    *out = (mi_page_ref){k, id};
    return MI_OK;
}

mi_status mi_kv_sequence_retain_page(const mi_kv_sequence *s, size_t page, mi_page_ref *out) {
    if (!s || !out)
        return MI_EINVAL;
    mi_paged_kv *k = s->kv;
    pthread_mutex_lock(&k->mutex);
    mi_status st = page < s->count ? retain_sealed(k, s->pages[page], out) : MI_EINVAL;
    pthread_mutex_unlock(&k->mutex);
    return st;
}

mi_status mi_page_ref_clone(const mi_page_ref *ref, mi_page_ref *out) {
    if (!ref || !ref->kv || !out)
        return MI_EINVAL;
    mi_paged_kv *k = ref->kv;
    pthread_mutex_lock(&k->mutex);
    mi_status st = retain_sealed(k, ref->page, out);
    pthread_mutex_unlock(&k->mutex);
    return st;
}

mi_block_id mi_page_ref_block(const mi_page_ref *ref, size_t layer) {
    return ref && ref->kv && layer < ref->kv->layers
        ? page_blocks(ref->kv, ref->page)[layer] : MI_INVALID_BLOCK_ID;
}

void mi_page_ref_release(mi_page_ref *ref) {
    if (!ref || !ref->kv)
        return;
    mi_paged_kv *k = ref->kv;
    pthread_mutex_lock(&k->mutex);
    page_release(k, ref->page);
    pthread_mutex_unlock(&k->mutex);
    *ref = (mi_page_ref){0};
}

int mi_page_ref_idle_leaf(const mi_page_ref *ref, uint64_t *last_used) {
    mi_paged_kv *k = ref->kv;
    pthread_mutex_lock(&k->mutex);
    /* Only the index and the export itself may own references at selection. */
    int idle = idle_leaf(k, ref->page, 2);
    if (idle)
        *last_used = k->pages[ref->page].last_used;
    pthread_mutex_unlock(&k->mutex);
    return idle;
}

int mi_page_ref_evict_leaf(mi_page_ref *ref) {
    mi_paged_kv *k = ref->kv;
    pthread_mutex_lock(&k->mutex);
    /* A sequence may have looked up this page during the control RPC. */
    int idle = idle_leaf(k, ref->page, 2);
    if (idle) {
        unlink_leaf(k, ref->page);
        page_release(k, ref->page);
        *ref = (mi_page_ref){0};
    }
    pthread_mutex_unlock(&k->mutex);
    return idle;
}

mi_status mi_paged_kv_acquire(mi_kv_sequence *s, size_t page, size_t layer,
                              mi_access_mode mode, mi_stream stream, mi_page_lease *out) {
    if (!s || !out || layer >= s->kv->layers)
        return MI_EINVAL;
    *out = (mi_page_lease){0};
    mi_paged_kv *k = s->kv;
    pthread_mutex_lock(&k->mutex);
    if (page >= s->count) {
        pthread_mutex_unlock(&k->mutex);
        return MI_EINVAL;
    }
    uint32_t id = s->pages[page];
    mi_status st = mode == MI_ACCESS_WRITE && k->pages[id].sealed ? MI_ESTATE
        : mi_storage_acquire(k->storage, page_blocks(k, id)[layer], mode, stream, &out->lease);
    if (st == MI_OK) {
        ++k->pages[id].refs;
        out->kv = k;
        out->page = id;
    }
    pthread_mutex_unlock(&k->mutex);
    return st;
}

static mi_status page_complete(mi_page_lease *lease, int commit) {
    mi_paged_kv *k = lease->kv;
    mi_status st = commit ? mi_lease_finish(&lease->lease) : mi_lease_abort(&lease->lease);
    if (!lease->lease.active) {
        pthread_mutex_lock(&k->mutex);
        page_release(k, lease->page);
        pthread_mutex_unlock(&k->mutex);
        lease->kv = NULL;
    }
    return st;
}

mi_status mi_page_lease_finish(mi_page_lease *lease) {
    return lease && lease->kv ? page_complete(lease, 1) : MI_EINVAL;
}

mi_status mi_page_lease_abort(mi_page_lease *lease) {
    return lease && lease->kv ? page_complete(lease, 0) : MI_EINVAL;
}

/* 先检查各层数据已提交，再发布完整页前缀。 */
mi_status mi_kv_sequence_publish(mi_kv_sequence *s, const int32_t *tokens,
                                 size_t count, size_t pages) {
    if (!s || !tokens || pages > count / s->kv->tokens_per_page)
        return MI_EINVAL;
    mi_paged_kv *k = s->kv;
    pthread_mutex_lock(&k->mutex);
    mi_status st = MI_EINVAL;
    if (pages > s->count)
        goto done;
    st = MI_OK;
    for (size_t i = 0; i < pages; ++i) {
        uint32_t id = s->pages[i];
        mi_kv_page *p = &k->pages[id];
        const int32_t *edge = tokens + i * k->tokens_per_page;
        if (p->sealed) {
            if (memcmp(page_tokens(k, id), edge, k->tokens_per_page * sizeof(*tokens))) {
                st = MI_ESTATE;
                goto done;
            }
        } else {
            st = mi_storage_blocks_ready(k->storage, page_blocks(k, id), k->layers);
            if (st != MI_OK)
                goto done;
        }
    }

    uint32_t parent = 0;
    for (size_t i = 0; i < pages; ++i) {
        uint32_t id = s->pages[i];
        mi_kv_page *p = &k->pages[id];
        const int32_t *edge = tokens + i * k->tokens_per_page;
        uint64_t hash = edge_hash(parent, edge, k->tokens_per_page);
        size_t slot = prefix_slot(k, parent, hash, edge);
        if (!p->sealed) {
            memcpy(page_tokens(k, id), edge, k->tokens_per_page * sizeof(*tokens));
            /* 发布后的页可共享，后续只允许读取。 */
            p->sealed = 1;
        }
        if (!k->prefix_buckets[slot]) {
            if (p->linked)
                break;
            p->linked = 1;
            p->parent = parent;
            p->edge_hash = hash;
            if (parent)
                ++k->pages[parent - 1].children;
            /* 前缀索引持有独立引用，序列销毁后仍可命中。 */
            ++p->refs;
            k->prefix_buckets[slot] = id + 1;
        }
        parent = k->prefix_buckets[slot];
        k->pages[parent - 1].last_used = ++k->prefix_clock;
    }

done:
    pthread_mutex_unlock(&k->mutex);
    return st;
}

/* 沿前缀索引匹配完整页，并为命中页保留引用。 */
mi_status mi_paged_kv_match(mi_paged_kv *k, const int32_t *tokens, size_t count,
                            mi_kv_sequence **out, size_t *matched) {
    if (!k || !tokens || !out)
        return MI_EINVAL;
    mi_kv_sequence *s;
    mi_status st = mi_kv_sequence_create(k, &s);
    if (st != MI_OK)
        return st;

    pthread_mutex_lock(&k->mutex);
    uint32_t parent = 0;
    size_t pages = count / k->tokens_per_page;
    for (size_t i = 0; i < pages; ++i) {
        const int32_t *edge = tokens + i * k->tokens_per_page;
        uint64_t hash = edge_hash(parent, edge, k->tokens_per_page);
        uint32_t found = k->prefix_buckets[prefix_slot(k, parent, hash, edge)];
        if (!found)
            break;
        uint32_t id = found - 1;
        ++k->pages[id].refs;
        k->pages[id].last_used = ++k->prefix_clock;
        s->pages[s->count++] = id;
        /* 用当前命中页作为下一次查找的父页。 */
        parent = found;
    }
    pthread_mutex_unlock(&k->mutex);
    *out = s;
    if (matched)
        *matched = s->count * k->tokens_per_page;
    return MI_OK;
}

mi_status mi_paged_kv_evict_prefixes(mi_paged_kv *k) {
    if (!k)
        return MI_EINVAL;
    pthread_mutex_lock(&k->mutex);
    memset(k->prefix_buckets, 0, (k->prefix_mask + 1) * sizeof(*k->prefix_buckets));
    for (size_t i = 0; i < k->max_pages; ++i) {
        mi_kv_page *p = &k->pages[i];
        if (!p->linked)
            continue;
        p->linked = 0;
        p->children = 0;
        page_release(k, (uint32_t)i);
    }
    pthread_mutex_unlock(&k->mutex);
    return MI_OK;
}

void mi_kv_sequence_destroy(mi_kv_sequence *s) {
    if (!s)
        return;
    mi_paged_kv *k = s->kv;
    pthread_mutex_lock(&k->mutex);
    for (size_t i = 0; i < s->count; ++i)
        page_release(k, s->pages[i]);
    pthread_mutex_unlock(&k->mutex);
    free(s);
}

void mi_paged_kv_destroy(mi_paged_kv *k) {
    if (!k)
        return;
    (void)mi_paged_kv_evict_prefixes(k);
    pthread_mutex_destroy(&k->mutex);
    free(k->prefix_buckets);
    free(k->tokens);
    free(k->blocks);
    free(k->free_ids);
    free(k->pages);
    free(k);
}
