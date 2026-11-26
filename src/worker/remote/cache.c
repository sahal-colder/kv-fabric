/*
 * Remote Connector for whole pages. Worker owns exports. Each active export
 * holds a sealed page ref until retirement. A resolving sequence owns its suffix.
 * Lock order: updates -> exports_lock -> Trie -> Storage -> Backend. Only
 * the publication serialization lock spans control RPCs.
 */
#include "internal.h"
#include "../tiered_internal.h"
#include <string.h>

static kvf_status export_page(mi_remote_worker *w, mi_kv_sequence *s, size_t page,
                              kvf_page_key key, kvf_hash parent, const kvf_manifest *verified,
                              const atomic_bool *cancel) {
    mi_remote_export *entry = mi_remote_find_export(w, key);
    if (entry && entry->state != EXPORT_FAILED) {
        pthread_mutex_lock(&w->exports_lock);
        int hidden = entry->hidden || entry->state == EXPORT_RETIRING;
        if (hidden)
            entry->state = EXPORT_RETIRING;
        pthread_mutex_unlock(&w->exports_lock);
        if (!hidden)
            return KVF_OK;
        kvf_status retired = kvf_client_invalidate(w->control, &entry->manifest);
        if (retired != KVF_OK && retired != KVF_NOT_FOUND && retired != KVF_STALE)
            return retired;
        mi_remote_drop_export(w, entry, EXPORT_EMPTY);
    }
    if (w->publication_order == UINT64_MAX)
        return KVF_LIMIT;
    uint64_t order = ++w->publication_order;
    for (uint32_t i = 0; !entry && i < w->exports_capacity; ++i) {
        if (w->exports[i].state == EXPORT_EMPTY || w->exports[i].state == EXPORT_FAILED) {
            entry = &w->exports[i];
        }
    }
    if (!entry)
        return KVF_LIMIT;

    mi_remote_export ready = {
        .state = EXPORT_PENDING, .order = order,
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
    kvf_status digest = kvf_manifest_digest(&ready.manifest, &ready.digest);
    if (digest != KVF_OK) {
        mi_page_ref_release(&ready.ref);
        return digest;
    }
    pthread_mutex_lock(&w->exports_lock);
    /* No handler can observe a manifest with only some layers filled in. */
    int cancelled = mi_remote_cancelled(cancel);
    if (!cancelled)
        *entry = ready;
    pthread_mutex_unlock(&w->exports_lock);
    if (cancelled)
        mi_page_ref_release(&ready.ref);
    return cancelled ? KVF_CANCELLED : KVF_OK;
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
            st = export_page(w, s, page, key, parent, NULL, NULL);
    }
    kvf_status sync = mi_remote_sync_locked(w);
    pthread_mutex_unlock(&w->updates);
    return st == KVF_OK ? sync : st;
}

/* The caller serializes mutations with updates. Existing serving references can
 * finish their local copy, but Directory validation fences retired revisions. */
void mi_remote_drop_export(mi_remote_worker *w, mi_remote_export *entry, export_state state) {
    pthread_mutex_lock(&w->exports_lock);
    entry->state = state;
    mi_page_ref ref = entry->ref;
    entry->ref = (mi_page_ref){0};
    pthread_mutex_unlock(&w->exports_lock);
    mi_page_ref_release(&ref);
    if (state != EXPORT_FAILED)
        atomic_fetch_add_explicit(&w->evictions, 1, memory_order_relaxed);
}

static kvf_status make_export_room(mi_remote_worker *w) {
    for (uint32_t i = 0; i < w->exports_capacity; ++i)
        if (w->exports[i].state == EXPORT_EMPTY || w->exports[i].state == EXPORT_FAILED)
            return KVF_OK;

    uint64_t after = 0;
    for (;;) {
        mi_remote_export *oldest = NULL;
        for (uint32_t i = 0; i < w->exports_capacity; ++i) {
            mi_remote_export *e = &w->exports[i];
            if ((e->state == EXPORT_ACTIVE || e->state == EXPORT_RETRY || e->state == EXPORT_PENDING) &&
                e->order > after &&
                (!oldest || e->order < oldest->order))
                oldest = e;
        }
        if (!oldest)
            return KVF_LIMIT;
        after = oldest->order;
        kvf_status st = kvf_client_retire(w->control, &oldest->manifest);
        if (st == KVF_CONFLICT)
            continue;
        if (st != KVF_OK && st != KVF_NOT_FOUND && st != KVF_STALE)
            return st;
        mi_remote_drop_export(w, oldest, EXPORT_EMPTY);
        return KVF_OK;
    }
}

kvf_status mi_remote_publish_page_guarded(mi_remote_worker *w, mi_kv_sequence *s,
                                           kvf_hash parent, const int32_t *tokens,
                                           const atomic_bool *cancel, int *retained) {
    if (retained)
        *retained = 0;
    if (!w || !s || s->kv != w->cache || !tokens || mi_kv_sequence_page_count(s) != 1)
        return KVF_INVALID;
    kvf_page_key key = {.space = w->space};
    kvf_status st = kvf_page_hash(&w->identity, parent, tokens, w->identity.tokens_per_page, &key.page);
    if (st != KVF_OK)
        return st;
    st = mi_remote_from_storage(mi_kv_sequence_seal_page(s, 0, tokens));
    if (st != KVF_OK)
        return st;
    pthread_mutex_lock(&w->updates);
    if (mi_remote_cancelled(cancel))
        st = KVF_CANCELLED;
    if (st == KVF_OK && !mi_remote_find_export(w, key))
        st = make_export_room(w);
    if (st == KVF_OK)
        st = export_page(w, s, 0, key, parent, NULL, cancel);
    if (st == KVF_OK) {
        kvf_status sync = mi_remote_sync_locked(w);
        mi_remote_export *entry = mi_remote_find_export(w, key);
        if (entry && entry->state != EXPORT_FAILED && entry->state != EXPORT_RETIRING && retained)
            *retained = 1;
        if (mi_remote_cancelled(cancel))
            st = KVF_CANCELLED;
        else if (entry && entry->state == EXPORT_ACTIVE)
            st = KVF_OK;
        else
            st = entry && entry->status != KVF_OK ? entry->status : sync;
    }
    pthread_mutex_unlock(&w->updates);
    return st;
}

kvf_status mi_remote_publish_page(mi_remote_worker *w, mi_kv_sequence *s,
                                   kvf_hash parent, const int32_t *tokens) {
    return mi_remote_publish_page_guarded(w, s, parent, tokens, NULL, NULL);
}

uint64_t mi_remote_hide_page_revision(mi_remote_worker *w, kvf_hash page) {
    if (!w || kvf_hash_zero(page))
        return 0;
    pthread_mutex_lock(&w->exports_lock);
    mi_remote_export *entry = mi_remote_find_export(w, (kvf_page_key){w->space, page});
    uint64_t revision = entry ? entry->order : 0;
    if (entry)
        entry->hidden = 1;
    pthread_mutex_unlock(&w->exports_lock);
    return revision;
}

void mi_remote_hide_page(mi_remote_worker *w, kvf_hash page) {
    (void)mi_remote_hide_page_revision(w, page);
}

/* revision=0 keeps the synchronous legacy API; queued work must pass its
 * captured nonzero revision and cannot invalidate a replacement publication. */
kvf_status mi_remote_invalidate_page_revision(mi_remote_worker *w, kvf_hash page, uint64_t revision) {
    if (!w || kvf_hash_zero(page))
        return KVF_INVALID;
    pthread_mutex_lock(&w->updates);
    mi_remote_export *e = mi_remote_find_export(w, (kvf_page_key){w->space, page});
    kvf_status st = KVF_NOT_FOUND;
    if (e && (!revision || revision == e->order) && e->state != EXPORT_FAILED) {
        pthread_mutex_lock(&w->exports_lock);
        e->state = EXPORT_RETIRING;
        e->hidden = 1;
        pthread_mutex_unlock(&w->exports_lock);
        st = kvf_client_invalidate(w->control, &e->manifest);
        if (st == KVF_OK || st == KVF_NOT_FOUND || st == KVF_STALE)
            mi_remote_drop_export(w, e, EXPORT_EMPTY);
    } else if (e && revision && revision != e->order) {
        st = KVF_STALE;
    }
    pthread_mutex_unlock(&w->updates);
    return st;
}

kvf_status mi_remote_invalidate_page(mi_remote_worker *w, kvf_hash page) {
    return mi_remote_invalidate_page_revision(w, page, 0);
}

uint64_t mi_remote_worker_evictions(const mi_remote_worker *w) {
    return atomic_load_explicit(&w->evictions, memory_order_relaxed);
}

kvf_status mi_remote_append_page(mi_remote_worker *w, mi_kv_sequence *s) {
    if (!w || !s || s->kv != w->cache)
        return KVF_INVALID;
    mi_status local = mi_kv_sequence_append_page(s, w->location);
    if (local != MI_ENOMEM || mi_kv_sequence_page_count(s) == w->cache->max_pages)
        return mi_remote_from_storage(local);

    pthread_mutex_lock(&w->updates);
    kvf_status st = mi_remote_from_storage(mi_kv_sequence_append_page(s, w->location));
    if (st != KVF_LIMIT)
        goto done;
    uint64_t after = 0;
    for (uint32_t attempt = 0; attempt < w->exports_capacity; ++attempt) {
        mi_remote_export *oldest = NULL;
        uint64_t age = UINT64_MAX;
        for (uint32_t i = 0; i < w->exports_capacity; ++i) {
            mi_remote_export *e = &w->exports[i];
            uint64_t used;
            if ((e->state == EXPORT_ACTIVE || e->state == EXPORT_RETRY || e->state == EXPORT_PENDING) &&
                mi_page_ref_idle_leaf(&e->ref, &used) &&
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
            oldest->state = EXPORT_RETRY;
            pthread_cond_signal(&w->wake);
        }
        pthread_mutex_unlock(&w->exports_lock);
        if (!evicted)
            continue;
        st = mi_remote_from_storage(mi_kv_sequence_append_page(s, w->location));
        if (st != KVF_LIMIT)
            break;
    }
done:
    pthread_mutex_unlock(&w->updates);
    return st;
}

kvf_status mi_remote_evict_prefixes(mi_remote_worker *w) {
    if (!w)
        return KVF_INVALID;
    pthread_mutex_lock(&w->updates);
    pthread_mutex_lock(&w->exports_lock);
    for (uint32_t i = 0; i < w->exports_capacity; ++i)
        if (w->exports[i].state != EXPORT_EMPTY && w->exports[i].state != EXPORT_FAILED)
            w->exports[i].state = EXPORT_RETIRING;
    pthread_mutex_unlock(&w->exports_lock);
    for (uint32_t i = 0; i < w->exports_capacity; ++i)
        mi_page_ref_release(&w->exports[i].ref);
    (void)mi_paged_kv_evict_prefixes(w->cache);
    kvf_status st = mi_remote_sync_locked(w);
    pthread_mutex_unlock(&w->updates);
    return st;
}

static kvf_status release_grant(mi_remote_worker *w, kvf_ticket ticket, uint64_t deadline_ms,
                                const atomic_bool *cancel) {
    kvf_client client = w->control;
    if (deadline_ms && mi_remote_request_client(w->control, deadline_ms, cancel, &client) != KVF_OK &&
        client.timeout_ms > 100)
        client.timeout_ms = 100; /* Bounded cleanup after the request has expired or been cancelled. */
    return kvf_client_release(client, ticket);
}

static kvf_status import_page(mi_remote_worker *w, mi_kv_sequence *s, size_t page,
                              const kvf_hash *candidates, uint32_t remaining, kvf_hash parent,
                              const int32_t *tokens, size_t count, mi_remote_result *result,
                              mi_remote_suffix suffix, const atomic_bool *cancel) {
    kvf_client client;
    kvf_status st = mi_remote_request_client(w->control, suffix.deadline_ms, cancel, &client);
    if (st != KVF_OK)
        return st;
    kvf_grant g;
    ++result->lookup_requests;
    st = kvf_client_acquire_prefix(client, w->space, candidates, remaining, &g);
    if (st != KVF_OK)
        return st;
    if (!kvf_hash_equal(g.manifest.parent, parent)) {
        st = KVF_PROTOCOL;
    } else {
        ++result->lookup_hits;
        st = suffix.deadline_ms ? mi_remote_from_storage(mi_kv_sequence_append_page(s, w->location))
                               : mi_remote_append_page(w, s);
    }
    uint64_t start = kvf_now_ns();
    if (st == KVF_OK)
        st = mi_remote_import(w, s, page, &g, suffix.deadline_ms, cancel);
    uint64_t end = kvf_now_ns();
    kvf_status released = release_grant(w, g.ticket, suffix.deadline_ms, cancel);
    if (st == KVF_OK)
        st = released;
    if (st == KVF_OK)
        st = mi_remote_request_client(w->control, suffix.deadline_ms, cancel, NULL);
    if (st == KVF_OK && !suffix.deadline_ms) {
        pthread_mutex_lock(&w->updates);
        st = mi_remote_cancelled(cancel) ? KVF_CANCELLED
            : mi_remote_from_storage(mi_kv_sequence_publish(s, tokens, count, page + 1));
        if (st == KVF_OK &&
            export_page(w, s, page, g.manifest.key, parent, &g.manifest, NULL) == KVF_OK)
            pthread_cond_signal(&w->wake);
        pthread_mutex_unlock(&w->updates);
    }
    /* The failed page may have some committed local blocks, but no consumer
     * can see them until all layers are committed and the Trie edge is linked. */
    if (st != KVF_OK)
        (void)mi_kv_sequence_truncate(s, page);
    else if (start && end >= start) {
        uint64_t duration = end - start;
        if (suffix.page_transfer_ns)
            suffix.page_transfer_ns[result->transfer_samples] = duration;
        result->transfer_ns += duration;
        ++result->transfer_samples;
    }
    return st;
}

static kvf_status resolve_suffix(mi_remote_worker *w, mi_kv_sequence *s,
                                 const int32_t *tokens, size_t count, mi_remote_result *result,
                                 mi_remote_suffix suffix, const atomic_bool *cancel) {
    const uint32_t n = w->identity.tokens_per_page;
    size_t page = result->local_tokens / n;
    size_t end = count / n;
    kvf_hash parent = suffix.parent;
    for (size_t i = 0; i < page; ++i) {
        kvf_status st = kvf_page_hash(&w->identity, parent, tokens + i * n, n, &parent);
        if (st != KVF_OK)
            return st;
    }

    while (page < end) {
        kvf_status st = mi_remote_request_client(w->control, suffix.deadline_ms, cancel, NULL);
        if (st != KVF_OK)
            return st;
        kvf_hash candidates[KVF_MAX_LOOKUP];
        uint32_t ask = end - page < KVF_MAX_LOOKUP ? (uint32_t)(end - page) : KVF_MAX_LOOKUP;
        kvf_hash tail = parent;
        for (uint32_t i = 0; i < ask; ++i) {
            st = kvf_page_hash(&w->identity, tail, tokens + (page + i) * n, n, &candidates[i]);
            if (st != KVF_OK)
                return st;
            tail = candidates[i];
        }
        /* ACQUIRE_PREFIX finds the best live copy of the first missing page
         * using the remaining window, so a separate lookup RPC is unnecessary. */
        for (uint32_t i = 0; i < ask; ++i) {
            st = import_page(w, s, page, candidates + i, ask - i, parent, tokens, count,
                             result, suffix, cancel);
            if (st != KVF_OK)
                return st;
            parent = candidates[i];
            ++page;
            result->remote_tokens += n;
        }
    }
    return KVF_OK;
}

static kvf_status stage_batch_page(mi_remote_worker *w, mi_kv_sequence *s, size_t page,
                                    const kvf_manifest *manifest, const unsigned char *data,
                                    mi_remote_timing *timing) {
    size_t bytes = w->identity.bytes_per_layer;
    uint64_t start = kvf_now_ns();
    for (uint32_t l = 0; l < w->identity.layers; ++l) {
        if (kvf_crc32c(data + l * bytes, bytes) != manifest->layer[l].checksum) {
            timing->crc_verify_ns += kvf_now_ns() - start;
            return KVF_CRC;
        }
    }
    timing->crc_verify_ns += kvf_now_ns() - start;
    start = kvf_now_ns();
    mi_status st = mi_kv_sequence_append_page(s, w->location);
    timing->kv_commit_ns += kvf_now_ns() - start;
    for (uint32_t l = 0; st == MI_OK && l < w->identity.layers; ++l) {
        mi_page_lease lease;
        start = kvf_now_ns();
        st = mi_paged_kv_acquire(s, page, l, MI_ACCESS_WRITE, NULL, &lease);
        timing->kv_commit_ns += kvf_now_ns() - start;
        if (st != MI_OK)
            break;
        start = kvf_now_ns();
        memcpy(lease.lease.data, data + l * bytes, bytes);
        timing->cpu_stage_ns += kvf_now_ns() - start;
        start = kvf_now_ns();
        st = mi_page_lease_finish(&lease);
        if (lease.kv)
            (void)mi_page_lease_abort(&lease);
        timing->kv_commit_ns += kvf_now_ns() - start;
    }
    return mi_remote_from_storage(st);
}

static kvf_status resolve_batch(mi_remote_worker *w, mi_kv_sequence *s, const int32_t *tokens,
                                size_t count, mi_remote_result *result, mi_remote_suffix suffix,
                                const atomic_bool *cancel) {
    const uint32_t n = w->identity.tokens_per_page;
    const size_t page_bytes = (size_t)w->identity.layers * w->identity.bytes_per_layer;
    size_t limit = KVF_MAX_BATCH_BYTES / page_bytes;
    if (limit > KVF_MAX_BATCH_PAGES)
        limit = KVF_MAX_BATCH_PAGES;
    kvf_hash parent = suffix.parent;
    mi_remote_timing *timing = &result->timing;
    for (size_t page = 0; page < count / n;) {
        kvf_client client;
        kvf_status st = mi_remote_request_client(w->control, suffix.deadline_ms, cancel, &client);
        if (st != KVF_OK)
            return st;
        size_t left = count / n - page;
        uint32_t ask = (uint32_t)(left < limit ? left : limit);
        kvf_hash candidates[KVF_MAX_BATCH_PAGES];
        kvf_hash tail = parent;
        uint64_t start = kvf_now_ns();
        for (uint32_t i = 0; i < ask; ++i) {
            st = kvf_page_hash(&w->identity, tail, tokens + (page + i) * n, n, &candidates[i]);
            if (st != KVF_OK)
                break;
            tail = candidates[i];
        }
        kvf_grant grants[KVF_MAX_BATCH_PAGES];
        uint32_t matched = 0;
        if (st == KVF_OK) {
            ++result->lookup_requests;
            st = kvf_client_acquire_batch(client, w->space, candidates, ask, grants, &matched);
        }
        timing->dir_lookup_ns += kvf_now_ns() - start;
        if (st != KVF_OK)
            return st;
        result->lookup_hits += matched;

        start = kvf_now_ns();
        kvf_batch_page fetch[KVF_MAX_BATCH_PAGES] = {0};
        for (uint32_t i = 0; i < matched; ++i) {
            const kvf_manifest *m = &grants[i].manifest;
            fetch[i] = (kvf_batch_page){.key = m->key, .owner_boot = m->owner.boot, .ticket = grants[i].ticket};
            if (!kvf_hash_equal(m->parent, i ? candidates[i - 1] : parent))
                st = KVF_PROTOCOL;
            kvf_status digest = kvf_manifest_digest(m, &fetch[i].manifest_digest);
            if (digest != KVF_OK)
                st = digest;
        }
        timing->dir_lookup_ns += kvf_now_ns() - start;

        /* The lookup is already paid. Compare future staging/transfer to the measured
         * savings for the actual hit, including the remaining prompt forward. */
        if (st == KVF_OK && !page && matched < ask && suffix.remote_ns_per_byte > 0) {
            size_t remaining = suffix.remaining_prompt_tokens;
            size_t after = remaining > (size_t)matched * n ? remaining - (size_t)matched * n : 1;
            if (remaining > after &&
                (remaining - after) * suffix.prefill_ns_per_token <
                    suffix.remote_fixed_ns + matched * page_bytes * suffix.remote_ns_per_byte)
                result->admission_skipped = 1;
        }
        for (uint32_t first = 0; st == KVF_OK && !result->admission_skipped && first < matched;) {
            uint32_t end = first + 1;
            while (end < matched && kvf_uuid_equal(fetch[first].owner_boot, fetch[end].owner_boot))
                ++end;
            st = mi_remote_request_client(w->control, suffix.deadline_ms, cancel, NULL);
            if (st == KVF_OK)
                st = mi_remote_receive_batch(w, &grants[first], fetch + first, end - first,
                                              (end - first) * page_bytes, suffix.deadline_ms, cancel, timing);
            for (uint32_t i = first; st == KVF_OK && i < end; ++i) {
                st = mi_remote_request_client(w->control, suffix.deadline_ms, cancel, NULL);
                start = kvf_now_ns();
                if (st == KVF_OK)
                    st = stage_batch_page(w, s, page + i, &grants[i].manifest,
                              (const unsigned char *)w->batch_recv.data + (i - first) * page_bytes, timing);
                if (st == KVF_OK) {
                    uint64_t elapsed = kvf_now_ns() - start;
                    if (suffix.page_transfer_ns)
                        suffix.page_transfer_ns[result->transfer_samples] = elapsed;
                    result->transfer_ns += elapsed;
                    ++result->transfer_samples;
                }
            }
            first = end;
        }
        start = kvf_now_ns();
        client = w->control;
        kvf_status live = mi_remote_request_client(w->control, suffix.deadline_ms, cancel, &client);
        if (live != KVF_OK && client.timeout_ms > 100)
            client.timeout_ms = 100;
        kvf_status finished = kvf_client_check_batch(client, fetch, matched, 1);
        timing->lease_check_ns += kvf_now_ns() - start;
        if (st == KVF_OK)
            st = live == KVF_OK ? finished : live;
        if (st != KVF_OK)
            return st;
        if (result->admission_skipped)
            return KVF_OK;
        /* FINISH validated and released this batch. Committed local snapshots
         * no longer borrow a remote lease; only the request deadline remains. */
        result->valid_until_ms = 0;
        result->remote_tokens += (size_t)matched * n;
        page += matched;
        parent = candidates[matched - 1];
        if (matched < ask)
            return KVF_NOT_FOUND;
    }
    return mi_remote_request_client(w->control, suffix.deadline_ms, cancel, NULL);
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
        result->remote_status = resolve_suffix(w, *out, tokens, count, result, (mi_remote_suffix){0}, cancel);
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

kvf_status mi_remote_resolve_after(mi_remote_worker *w, mi_remote_suffix suffix, const int32_t *tokens,
                                    size_t count, mi_kv_sequence **out, mi_remote_result *result,
                                    const atomic_bool *cancel) {
    *result = (mi_remote_result){0};
    *out = NULL;
    kvf_status st = mi_remote_request_client(w->control, suffix.deadline_ms, cancel, NULL);
    if (st != KVF_OK)
        return st;
    st = mi_remote_from_storage(mi_kv_sequence_create(w->cache, out));
    if (st != KVF_OK)
        return st;
    size_t page_bytes = (size_t)w->identity.layers * w->identity.bytes_per_layer;
    result->remote_status = w->location == MI_LOCATION_CPU && page_bytes <= KVF_MAX_BATCH_BYTES
        ? resolve_batch(w, *out, tokens, count, result, suffix, cancel)
        : resolve_suffix(w, *out, tokens, count, result, suffix, cancel);
    if (result->remote_status != KVF_OK && result->remote_status != KVF_NOT_FOUND) {
        /* The scheduler cannot use a batch that failed while staging. Return
         * its slots before the single prefetch lane takes the next request. */
        mi_kv_sequence_destroy(*out);
        *out = NULL;
        result->remote_tokens = 0;
        if (result->remote_status != KVF_CANCELLED)
            atomic_fetch_add_explicit(&w->failed_fetches, 1, memory_order_relaxed);
        return KVF_OK;
    }
    if (result->remote_tokens) {
        atomic_fetch_add_explicit(&w->remote_hits, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&w->reused_tokens, result->remote_tokens, memory_order_relaxed);
    }
    return KVF_OK;
}

kvf_status mi_remote_resolve(mi_remote_worker *w, const int32_t *tokens, size_t count,
                              mi_kv_sequence **out, mi_remote_result *result) {
    return mi_remote_resolve_impl(w, tokens, count, out, result, NULL);
}
