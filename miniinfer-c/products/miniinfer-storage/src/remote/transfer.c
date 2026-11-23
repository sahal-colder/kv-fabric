/*
 * Peer transfer layer. The source owns one staging segment per active request.
 * The destination owns temporary WRITE leases and two DMA segments per importer.
 * Order: exports_lock -> Trie, then release both before Storage snapshot and TCP I/O.
 */
#include "internal.h"
#include "../tiered_internal.h"

static kvf_status copy_export(mi_remote_worker *w, mi_segment *stage, const kvf_fetch_request *r) {
    mi_page_ref ref = {0};
    pthread_mutex_lock(&w->exports_lock);
    mi_remote_export *e = mi_remote_find_export(w, r->key);
    kvf_status st = KVF_STALE;
    if (e && e->state == EXPORT_ACTIVE && r->layer_index < e->manifest.layer_count &&
        kvf_layer_equal(r->layer, e->manifest.layer[r->layer_index]))
        st = mi_remote_from_storage(mi_page_ref_clone(&e->ref, &ref));
    pthread_mutex_unlock(&w->exports_lock);
    if (st != KVF_OK)
        return st;

    /* The export may be retired once the lock is dropped. The cloned page keeps
     * its IDs alive while Storage acquires the separate, migration-blocking READ pin. */
    mi_block_meta meta;
    mi_block_id id = mi_page_ref_block(&ref, r->layer_index);
    if (id != r->layer.id || (uint32_t)(id >> 32) != r->layer.generation) {
        st = KVF_STALE;
    } else {
        st = mi_remote_from_storage(mi_storage_read_snapshot_event(w->storage, id, r->layer.version,
                                                                   stage->data, r->layer.size,
                                                                   stage->stream, stage->event, &meta));
    }
    mi_page_ref_release(&ref);
    if (st == KVF_OK && kvf_crc32c(stage->data, r->layer.size) != r->layer.checksum)
        st = KVF_CRC;
    return st;
}

static kvf_status validate(mi_remote_worker *w, const kvf_fetch_request *r, uint64_t deadline) {
    uint64_t now = kvf_now_ms();
    if (deadline <= now)
        return KVF_TIMEOUT;
    kvf_client c = w->control;
    if (deadline - now < c.timeout_ms)
        c.timeout_ms = (uint32_t)(deadline - now);
    return kvf_client_validate(c, r);
}

static kvf_status send_layer(mi_remote_worker *w, mi_segment *stage, kvf_frame f,
                              uint8_t *bytes, size_t capacity, int fd, uint64_t deadline) {
    kvf_reader reader = {.buffer = bytes, .size = f.size};
    kvf_fetch_request r;
    kvf_get_fetch(&reader, &r);
    f.response = 1;
    f.size = 0;
    f.status = KVF_INVALID;
    const void *payload = bytes;
    if (!kvf_reader_done(&reader) || r.layer_index >= w->identity.layers ||
        r.layer.size != w->identity.bytes_per_layer)
        return kvf_send_frame(fd, f, bytes, deadline);
    f.status = KVF_STALE;
    if (!kvf_uuid_equal(r.owner_boot, w->info.owner.boot) || !kvf_hash_equal(r.key.space, w->space))
        return kvf_send_frame(fd, f, bytes, deadline);

    f.status = validate(w, &r, deadline);
    if (f.status == KVF_OK)
        f.status = copy_export(w, stage, &r);
    /* A Directory restart or revoke may happen during the copy. Re-check before serving. */
    if (f.status == KVF_OK)
        f.status = validate(w, &r, deadline);
    if (f.status == KVF_OK && f.opcode == KVF_BLOCK_META) {
        kvf_writer result = {.buffer = bytes, .capacity = capacity};
        kvf_put_layer(&result, r.layer);
        f.size = (uint32_t)result.size;
    } else if (f.status == KVF_OK) {
        f.size = r.layer.size;
        payload = stage->data;
    }
    /* Only the independent staging Segment stays live during socket backpressure. */
    return kvf_send_frame(fd, f, payload, deadline);
}

void mi_remote_serve(void *context, unsigned lane, int fd, uint64_t deadline) {
    mi_remote_worker *w = context;
    uint8_t bytes[256];
    kvf_frame f;
    while (kvf_recv_frame(fd, &f, bytes, sizeof(bytes), deadline) == KVF_OK && !f.response) {
        if (f.opcode != KVF_FETCH && f.opcode != KVF_FETCH_STREAM && f.opcode != KVF_BLOCK_META)
            return;
        mi_segment *stage = mi_segment_acquire(w->segments, lane, lane + 1);
        if (!stage)
            return;
        atomic_fetch_add_explicit(&w->active_transfers, 1, memory_order_relaxed);
        kvf_status st = send_layer(w, stage, f, bytes, sizeof(bytes), fd, kvf_now_ms() + w->control.timeout_ms);
        atomic_fetch_sub_explicit(&w->active_transfers, 1, memory_order_relaxed);
        mi_segment_release(stage);
        if (st != KVF_OK || f.opcode != KVF_FETCH_STREAM)
            return;
        deadline = kvf_now_ms() + w->peer_idle_ms;
    }
}

static kvf_status finish_layer(mi_remote_worker *w, mi_segment *stage, mi_page_lease *lease) {
    if (!lease->kv)
        return KVF_OK;
    mi_status st = stage ? mi_device_event_wait(w->device, stage->event) : MI_OK;
    /* A failed event does not publish a write. Aborting also drains the corresponding stream. */
    mi_status retired = st == MI_OK ? mi_page_lease_finish(lease) : mi_page_lease_abort(lease);
    return mi_remote_from_storage(st == MI_OK ? retired : st);
}

kvf_status mi_remote_import(mi_remote_worker *w, mi_kv_sequence *s, size_t page,
                             const kvf_grant *g, const atomic_bool *cancel) {
    kvf_fetch_request r = {
        .key = g->manifest.key, .owner_boot = g->manifest.owner.boot, .ticket = g->ticket
    };
    mi_segment *stage[2] = {0};
    mi_page_lease leases[2] = {0};
    mi_peer_conn *peer = NULL;
    int fd = -1;
    kvf_status st = KVF_OK;
    unsigned stages = w->peers && g->manifest.layer_count > 1 ? 2 : 1;
    if (w->location == MI_LOCATION_GPU) {
        for (unsigned i = 0; i < stages; ++i) {
            stage[i] = mi_segment_acquire(w->segments, w->lanes + 1, w->segments->count);
            if (!stage[i]) {
                st = KVF_LIMIT;
                goto done;
            }
        }
    }
    if (w->peers) {
        st = mi_peer_borrow(w->peers, g, kvf_now_ms() + w->control.timeout_ms, &peer, &fd);
        if (st != KVF_OK)
            goto done;
    }

    uint64_t renew_at = kvf_now_ms() + g->ttl_ms / 2;
    for (uint32_t i = 0; i < g->manifest.layer_count; ++i) {
        if (mi_remote_cancelled(cancel)) {
            st = KVF_CANCELLED;
            goto done;
        }
        if (kvf_now_ms() >= renew_at) {
            uint32_t ttl_ms;
            st = kvf_client_renew(w->control, g->ticket, &ttl_ms);
            if (st != KVF_OK)
                goto done;
            renew_at = kvf_now_ms() + ttl_ms / 2;
        }
        unsigned slot = i % stages;
        mi_page_lease *lease = &leases[slot];
        st = finish_layer(w, stage[slot], lease);
        if (st != KVF_OK)
            goto done;
        mi_stream stream = stage[slot] ? stage[slot]->stream : NULL;
        st = mi_remote_from_storage(mi_paged_kv_acquire(s, page, i, MI_ACCESS_WRITE, stream, lease));
        if (st != KVF_OK)
            goto done;
        r.layer_index = i;
        r.layer = g->manifest.layer[i];
        void *dst = stage[slot] ? stage[slot]->data : lease->lease.data;
        st = fd >= 0 ? kvf_client_fetch_on(fd, kvf_now_ms() + w->control.timeout_ms,
                                           &r, dst, lease->lease.size)
                     : kvf_client_fetch(g->endpoint, w->control.timeout_ms, &r, dst, lease->lease.size);
        if (st != KVF_OK)
            goto done;
        if (stream) {
            mi_status local = mi_device_copy_from_host(w->device, lease->lease.data, dst, lease->lease.size, stream);
            if (local == MI_OK)
                local = mi_device_event_record(w->device, stage[slot]->event, stream);
            st = mi_remote_from_storage(local);
            /* Leave this WRITE lease pending while the other slot receives the next layer. */
        } else {
            st = mi_remote_from_storage(mi_page_lease_finish(lease));
        }
        if (st != KVF_OK)
            goto done;
    }
    for (unsigned i = 0; i < 2; ++i) {
        st = finish_layer(w, stage[i], &leases[i]);
        if (st != KVF_OK)
            goto done;
    }
    st = mi_remote_cancelled(cancel) ? KVF_CANCELLED : kvf_client_validate(w->control, &r);

done:
    for (unsigned i = 0; i < 2; ++i) {
        if (leases[i].kv)
            (void)mi_page_lease_abort(&leases[i]);
        mi_segment_release(stage[i]);
    }
    mi_peer_return(w->peers, peer, st == KVF_OK);
    return st;
}
