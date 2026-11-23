/*
 * Remote Connector for whole pages. Worker owns exports. Each active export
 * holds a sealed page ref until retirement. A resolving sequence owns its suffix.
 * Lock order: updates -> exports_lock -> Trie -> Storage -> Backend. Only
 * the publication serialization lock spans control RPCs.
 */
#include "internal.h"
#include "../tiered_internal.h"

static kvf_status export_page(mi_remote_worker *w, mi_kv_sequence *s, size_t page,
                              kvf_page_key key, kvf_hash parent, const kvf_manifest *verified) {
    mi_remote_export *entry = mi_remote_find_export(w, key);
    if (entry)
        return entry->state == EXPORT_ACTIVE ? KVF_OK : KVF_UNAVAILABLE;
    for (uint32_t i = 0; i < w->exports_capacity; ++i) {
        if (w->exports[i].state == EXPORT_EMPTY) {
            entry = &w->exports[i];
            break;
        }
    }
    if (!entry)
        return KVF_LIMIT;

    mi_remote_export ready = {
        .state = EXPORT_ACTIVE, .dirty = 1,
        .manifest = {.key = key, .parent = parent, .owner = w->info.owner, .identity = w->identity,
                     .token_count = w->identity.tokens_per_page, .layer_count = w->identity.layers}
    };
    mi_status local = mi_kv_sequence_retain_page(s, page, &ready.ref);
    if (local != MI_OK)
        return mi_remote_from_storage(local);
    mi_segment *stage = verified ? NULL : mi_segment_acquire(w->segments, w->lanes, w->lanes + 1);
    if (!verified && !stage) {
        mi_page_ref_release(&ready.ref);
        return KVF_LIMIT;
    }

    for (uint32_t i = 0; i < w->identity.layers; ++i) {
        mi_block_id id = mi_page_ref_block(&ready.ref, i);
        mi_block_meta meta;
        /* Imported bytes already passed network CRC and DMA completion. Reuse
         * their checksum but always publish the destination's own IDs/versions. */
        local = verified ? mi_storage_metadata(w->storage, id, &meta)
            : mi_storage_read_snapshot_event(w->storage, id, 0, stage->data, w->identity.bytes_per_layer,
                                              stage->stream, stage->event, &meta);
        if (local != MI_OK || meta.size != w->identity.bytes_per_layer) {
            mi_segment_release(stage);
            mi_page_ref_release(&ready.ref);
            return local == MI_OK ? KVF_INVALID : mi_remote_from_storage(local);
        }
        ready.manifest.layer[i] = (kvf_layer){
            .id = id, .version = meta.version, .generation = (uint32_t)(id >> 32),
            .size = (uint32_t)meta.size,
            .checksum = verified ? verified->layer[i].checksum : kvf_crc32c(stage->data, meta.size)
        };
    }
    mi_segment_release(stage);
    pthread_mutex_lock(&w->exports_lock);
    /* No handler can observe a manifest with only some layers filled in. */
    *entry = ready;
    pthread_mutex_unlock(&w->exports_lock);
    return KVF_OK;
}

kvf_status mi_remote_publish(mi_remote_worker *w, mi_kv_sequence *s,
                              const int32_t *tokens, size_t count, size_t complete_pages) {
    if (!w || !s || s->kv != w->cache || !tokens || !complete_pages ||
        complete_pages > count / w->identity.tokens_per_page)
        return KVF_INVALID;

    pthread_mutex_lock(&w->updates);
    kvf_status st = mi_remote_from_storage(mi_kv_sequence_publish(s, tokens, count, complete_pages));
    kvf_page_key key = {.space = w->space};
    for (size_t page = 0; st == KVF_OK && page < complete_pages; ++page) {
        uint32_t n = w->identity.tokens_per_page;
        kvf_hash parent = key.page;
        st = kvf_page_hash(&w->identity, parent, tokens + page * n, n, &key.page);
        if (st == KVF_OK)
            st = export_page(w, s, page, key, parent, NULL);
    }
    kvf_status sync = mi_remote_sync_locked(w);
    pthread_mutex_unlock(&w->updates);
    return st == KVF_OK ? sync : st;
}

kvf_status mi_remote_append_page(mi_remote_worker *w, mi_kv_sequence *s) {
    if (!w || !s || s->kv != w->cache)
        return KVF_INVALID;
    mi_status local = mi_kv_sequence_append_page(s, w->location);
    if (local != MI_ENOMEM || mi_kv_sequence_page_count(s) == w->cache->max_pages)
        return mi_remote_from_storage(local);

    pthread_mutex_lock(&w->updates);
    local = mi_kv_sequence_append_page(s, w->location);
    if (local != MI_ENOMEM) {
        pthread_mutex_unlock(&w->updates);
        return mi_remote_from_storage(local);
    }
    uint64_t after = 0;
    kvf_status st = KVF_LIMIT;
    for (uint32_t attempt = 0; attempt < w->exports_capacity; ++attempt) {
        mi_remote_export *oldest = NULL;
        uint64_t age = UINT64_MAX;
        for (uint32_t i = 0; i < w->exports_capacity; ++i) {
            mi_remote_export *e = &w->exports[i];
            uint64_t used;
            if (e->state == EXPORT_ACTIVE && mi_page_ref_idle_leaf(&e->ref, &used) &&
                used > after && used < age) {
                age = used;
                oldest = e;
            }
        }
        if (!oldest)
            break;
        after = age;
        /* Retire before releasing local refs so the Directory cannot issue a new lease
         * against the evicted replica. Existing leases veto automatic eviction. */
        kvf_status retired = kvf_client_retire(w->control, &oldest->manifest);
        if (retired == KVF_CONFLICT)
            continue;
        if (retired != KVF_OK && retired != KVF_NOT_FOUND && retired != KVF_STALE) {
            st = retired;
            break;
        }
        pthread_mutex_lock(&w->exports_lock);
        int evicted = mi_page_ref_evict_leaf(&oldest->ref);
        if (evicted) {
            *oldest = (mi_remote_export){0};
        } else {
            /* A local reader arrived during the RPC. Keep the ref and restore publication. */
            oldest->dirty = 1;
            pthread_cond_signal(&w->wake);
        }
        pthread_mutex_unlock(&w->exports_lock);
        if (!evicted)
            continue;
        st = mi_remote_from_storage(mi_kv_sequence_append_page(s, w->location));
        if (st != KVF_LIMIT)
            break;
    }
    pthread_mutex_unlock(&w->updates);
    return st;
}

kvf_status mi_remote_evict_prefixes(mi_remote_worker *w) {
    if (!w)
        return KVF_INVALID;
    pthread_mutex_lock(&w->updates);
    pthread_mutex_lock(&w->exports_lock);
    for (uint32_t i = 0; i < w->exports_capacity; ++i)
        if (w->exports[i].state == EXPORT_ACTIVE)
            w->exports[i].state = EXPORT_RETIRING;
    pthread_mutex_unlock(&w->exports_lock);
    for (uint32_t i = 0; i < w->exports_capacity; ++i)
        mi_page_ref_release(&w->exports[i].ref);
    (void)mi_paged_kv_evict_prefixes(w->cache);
    kvf_status st = mi_remote_sync_locked(w);
    pthread_mutex_unlock(&w->updates);
    return st;
}

static kvf_status import_page(mi_remote_worker *w, mi_kv_sequence *s, size_t page,
                              const kvf_hash *candidates, uint32_t remaining, kvf_hash parent,
                              const int32_t *tokens, size_t count, const atomic_bool *cancel) {
    if (mi_remote_cancelled(cancel))
        return KVF_CANCELLED;
    kvf_grant g;
    kvf_status st = kvf_client_acquire_prefix(w->control, w->space, candidates, remaining, &g);
    if (st != KVF_OK)
        return st;
    st = kvf_hash_equal(g.manifest.parent, parent) ? mi_remote_append_page(w, s) : KVF_PROTOCOL;
    if (st == KVF_OK)
        st = mi_remote_import(w, s, page, &g, cancel);
    kvf_status released = kvf_client_release(w->control, g.ticket);
    if (st == KVF_OK)
        st = released;
    if (st == KVF_OK && mi_remote_cancelled(cancel))
        st = KVF_CANCELLED;
    if (st == KVF_OK) {
        pthread_mutex_lock(&w->updates);
        st = mi_remote_cancelled(cancel) ? KVF_CANCELLED
            : mi_remote_from_storage(mi_kv_sequence_publish(s, tokens, count, page + 1));
        if (st == KVF_OK &&
            export_page(w, s, page, g.manifest.key, parent, &g.manifest) == KVF_OK)
            pthread_cond_signal(&w->wake);
        pthread_mutex_unlock(&w->updates);
    }
    /* The failed page may have some committed local blocks, but no consumer
     * can see them until all layers are committed and the Trie edge is linked. */
    if (st != KVF_OK)
        (void)mi_kv_sequence_truncate(s, page);
    return st;
}

static kvf_status resolve_suffix(mi_remote_worker *w, mi_kv_sequence *s,
                                 const int32_t *tokens, size_t count, mi_remote_result *result,
                                 const atomic_bool *cancel) {
    const uint32_t n = w->identity.tokens_per_page;
    size_t page = result->local_tokens / n;
    size_t end = count / n;
    kvf_hash parent = {0};
    for (size_t i = 0; i < page; ++i) {
        kvf_status st = kvf_page_hash(&w->identity, parent, tokens + i * n, n, &parent);
        if (st != KVF_OK)
            return st;
    }

    while (page < end) {
        if (mi_remote_cancelled(cancel))
            return KVF_CANCELLED;
        kvf_hash candidates[KVF_MAX_LOOKUP];
        uint32_t ask = end - page < KVF_MAX_LOOKUP ? (uint32_t)(end - page) : KVF_MAX_LOOKUP;
        kvf_hash tail = parent;
        for (uint32_t i = 0; i < ask; ++i) {
            kvf_status st = kvf_page_hash(&w->identity, tail, tokens + (page + i) * n, n, &candidates[i]);
            if (st != KVF_OK)
                return st;
            tail = candidates[i];
        }
        /* ACQUIRE_PREFIX finds the best live copy of the first missing page
         * using the remaining window, so a separate lookup RPC is unnecessary. */
        for (uint32_t i = 0; i < ask; ++i) {
            kvf_status st = import_page(w, s, page, candidates + i, ask - i, parent, tokens, count, cancel);
            if (st != KVF_OK)
                return st;
            parent = candidates[i];
            ++page;
            result->remote_tokens += n;
        }
    }
    return KVF_OK;
}

kvf_status mi_remote_resolve_impl(mi_remote_worker *w, const int32_t *tokens, size_t count,
                                   mi_kv_sequence **out, mi_remote_result *result,
                                   const atomic_bool *cancel) {
    if (!w || !tokens || !out || !result)
        return KVF_INVALID;
    *result = (mi_remote_result){0};
    *out = NULL;
    if (mi_remote_cancelled(cancel))
        return KVF_CANCELLED;
    kvf_status st = mi_remote_from_storage(mi_paged_kv_match(w->cache, tokens, count, out,
                                                           &result->local_tokens));
    if (st != KVF_OK)
        return st;
    if (result->local_tokens < count - count % w->identity.tokens_per_page)
        result->remote_status = resolve_suffix(w, *out, tokens, count, result, cancel);
    if (result->local_tokens)
        atomic_fetch_add_explicit(&w->local_hits, 1, memory_order_relaxed);
    if (result->remote_tokens)
        atomic_fetch_add_explicit(&w->remote_hits, 1, memory_order_relaxed);
    if (result->remote_status != KVF_OK && result->remote_status != KVF_NOT_FOUND &&
        result->remote_status != KVF_CANCELLED)
        atomic_fetch_add_explicit(&w->failed_fetches, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&w->reused_tokens, result->local_tokens + result->remote_tokens,
                              memory_order_relaxed);
    return KVF_OK;
}

kvf_status mi_remote_resolve(mi_remote_worker *w, const int32_t *tokens, size_t count,
                              mi_kv_sequence **out, mi_remote_result *result) {
    return mi_remote_resolve_impl(w, tokens, count, out, result, NULL);
}
