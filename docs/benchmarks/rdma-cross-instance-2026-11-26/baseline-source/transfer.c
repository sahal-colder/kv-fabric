/*
 * Peer transfer layer. The source owns one staging segment per active request.
 * The destination owns temporary WRITE leases and two DMA segments per importer.
 * Order: exports_lock -> Trie, then release both before Storage snapshot and network I/O.
 */
#include "internal.h"
#include "../tiered_internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#if KVF_HAS_RSOCKETS
#include <rdma/rsocket.h>
#endif

static kvf_status remote_send_all(mi_remote_socket s, const uint8_t *data, size_t size, int flags,
                                  uint64_t deadline) {
    while (size) {
        if (kvf_now_ms() >= deadline)
            return KVF_TIMEOUT;
        ssize_t n =
#if KVF_HAS_RSOCKETS
            s.transport == KVF_TRANSPORT_RDMA_RSOCKET ? rsend(s.fd, data, size, MSG_DONTWAIT | MSG_NOSIGNAL) :
#endif
            send(s.fd, data, size, MSG_DONTWAIT | MSG_NOSIGNAL | flags);
        if (n > 0) {
            data += n;
            size -= (size_t)n;
            continue;
        }
        if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
            return KVF_IO;
        if (errno != EINTR) {
            kvf_status st = remote_wait(s, POLLOUT, deadline);
            if (st != KVF_OK)
                return st;
        }
    }
    return KVF_OK;
}

static kvf_status remote_recv_all(mi_remote_socket s, uint8_t *data, size_t size, uint64_t deadline,
                                  uint64_t *received) {
    while (size) {
        if (kvf_now_ms() >= deadline)
            return KVF_TIMEOUT;
        ssize_t n =
#if KVF_HAS_RSOCKETS
            s.transport == KVF_TRANSPORT_RDMA_RSOCKET ? rrecv(s.fd, data, size, MSG_DONTWAIT) :
#endif
            recv(s.fd, data, size, MSG_DONTWAIT);
        if (n > 0) {
            data += n;
            size -= (size_t)n;
            if (received)
                *received += (uint64_t)n;
            continue;
        }
        if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
            return KVF_IO;
        if (errno != EINTR) {
            kvf_status st = remote_wait(s, POLLIN, deadline);
            if (st != KVF_OK)
                return st;
        }
    }
    return KVF_OK;
}

static kvf_status remote_send_frame(mi_remote_socket s, kvf_frame f, const void *payload, uint64_t end) {
    uint8_t header[KVF_HEADER_BYTES];
    kvf_status st = kvf_frame_encode_header(f, payload, header);
    if (st != KVF_OK)
        return st;
    /* Coalesce the TCP header and payload without copying them into one buffer. */
    st = remote_send_all(s, header, sizeof(header), f.size ? MSG_MORE : 0, end);
    return st == KVF_OK ? remote_send_all(s, payload, f.size, 0, end) : st;
}

static kvf_status remote_recv_frame(mi_remote_socket s, kvf_frame *f, void *payload, size_t capacity,
                                    uint64_t end, kvf_receive_timing *timing) {
    uint8_t header[KVF_HEADER_BYTES];
    uint64_t start = timing ? kvf_now_ns() : 0;
    MI_REMOTE_RANGE_BEGIN("kvfabric.wire.header_wait");
    kvf_status st = remote_recv_all(s, header, sizeof(header), end, NULL);
    MI_REMOTE_RANGE_END();
    if (timing) {
        uint64_t ns = kvf_now_ns() - start;
        timing->reply_wait_ns += ns;
        timing->net_recv_ns += ns;
    }
    if (st != KVF_OK)
        return st;
    uint32_t payload_crc;
    st = kvf_frame_decode_header(header, f, &payload_crc, timing ? &timing->crc_verify_ns : NULL);
    if (st != KVF_OK)
        return st;
    if (f->size > capacity)
        return KVF_PROTOCOL;
    start = timing ? kvf_now_ns() : 0;
    MI_REMOTE_RANGE_BEGIN("kvfabric.wire.body_receive");
    st = remote_recv_all(s, payload, f->size, end, timing ? &timing->received_bytes : NULL);
    MI_REMOTE_RANGE_END();
    if (timing)
        timing->net_recv_ns += kvf_now_ns() - start;
    if (st != KVF_OK)
        return st;
    start = timing ? kvf_now_ns() : 0;
    MI_REMOTE_RANGE_BEGIN("kvfabric.wire.payload_crc");
    uint32_t actual = kvf_crc32c(payload, f->size);
    MI_REMOTE_RANGE_END();
    if (timing)
        timing->crc_verify_ns += kvf_now_ns() - start;
    return actual == payload_crc ? KVF_OK : KVF_CRC;
}

static kvf_status remote_fetch(mi_remote_socket s, kvf_opcode op, const kvf_writer *request,
                               void *payload, size_t size, uint64_t end, kvf_receive_timing *timing) {
    static _Atomic uint64_t next = 1;
    if (request->failed)
        return KVF_INVALID;
    uint64_t id = atomic_fetch_add_explicit(&next, 1, memory_order_relaxed);
    kvf_frame f = {.opcode = op, .request_id = id, .size = (uint32_t)request->size};
    kvf_status st = remote_send_frame(s, f, request->buffer, end);
    if (st == KVF_OK)
        st = remote_recv_frame(s, &f, payload, size, end, timing);
    if (st != KVF_OK)
        return st;
    if (!f.response || f.opcode != op || f.request_id != id || (f.status == KVF_OK && f.size != size))
        return KVF_PROTOCOL;
    return f.status;
}

static kvf_status copy_export(mi_remote_worker *w, mi_segment *stage, const kvf_fetch_request *r) {
    mi_page_ref ref = {0};
    pthread_mutex_lock(&w->exports_lock);
    mi_remote_export *e = mi_remote_find_export(w, r->key);
    kvf_status st = KVF_STALE;
    if (mi_remote_export_visible(e) && r->layer_index < e->manifest.layer_count &&
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
    kvf_client client;
    kvf_status st = mi_remote_request_client(w->control, deadline, NULL, &client);
    return st == KVF_OK ? kvf_client_validate(client, r) : st;
}

static kvf_status send_layer(mi_remote_worker *w, mi_segment *stage, kvf_frame f,
                              uint8_t *bytes, size_t capacity, mi_remote_socket socket, uint64_t deadline) {
    kvf_reader reader = {.buffer = bytes, .size = f.size};
    kvf_fetch_request r;
    kvf_get_fetch(&reader, &r);
    f.response = 1;
    f.size = 0;
    f.status = KVF_INVALID;
    const void *payload = bytes;
    if (!kvf_reader_done(&reader) || r.layer_index >= w->identity.layers ||
        r.layer.size != w->identity.bytes_per_layer)
        goto reply;
    f.status = KVF_STALE;
    if (!kvf_uuid_equal(r.owner_boot, w->info.owner.boot) || !kvf_hash_equal(r.key.space, w->space))
        goto reply;

    f.status = validate(w, &r, deadline);
    if (f.status == KVF_OK)
        f.status = copy_export(w, stage, &r);
    /* A Directory restart or revoke may happen during the copy. Re-check before serving. */
    if (f.status == KVF_OK)
        f.status = validate(w, &r, deadline);
    if (f.status == KVF_OK) {
        pthread_mutex_lock(&w->exports_lock);
        mi_remote_export *e = mi_remote_find_export(w, r.key);
        if (!mi_remote_export_visible(e) || !kvf_layer_equal(r.layer, e->manifest.layer[r.layer_index]))
            f.status = KVF_STALE;
        pthread_mutex_unlock(&w->exports_lock);
    }
    if (f.status == KVF_OK && f.opcode == KVF_BLOCK_META) {
        kvf_writer result = {.buffer = bytes, .capacity = capacity};
        kvf_put_layer(&result, r.layer);
        f.size = (uint32_t)result.size;
    } else if (f.status == KVF_OK) {
        f.size = r.layer.size;
        payload = stage->data;
    }
reply:
    /* Only the independent staging Segment stays live during socket backpressure. */
    return remote_send_frame(socket, f, payload, deadline);
}

/* A cloned ref pins the block IDs. Snapshot reads verify Storage's live generation,
 * committed version and, on SSD, the stored record checksum. */
static kvf_status copy_batch_page(mi_remote_worker *w, mi_segment *stage, kvf_page_key key, const kvf_hash *digest,
                                  unsigned char *dst, uint64_t deadline) {
    mi_page_ref ref = {0};
    kvf_layer layers[KVF_MAX_LAYERS];
    pthread_mutex_lock(&w->exports_lock);
    mi_remote_export *e = mi_remote_find_export(w, key);
    kvf_status st = KVF_STALE;
    /* Self-reads only see committed local exports. Network reads can also
     * serve an acknowledged Directory lease during publication retries. */
    if (mi_remote_export_visible(e) &&
        (digest ? kvf_hash_equal(e->digest, *digest) : e->state == EXPORT_ACTIVE)) {
        st = mi_remote_from_storage(mi_page_ref_clone(&e->ref, &ref));
        for (uint32_t i = 0; i < w->identity.layers; ++i)
            layers[i] = e->manifest.layer[i];
    }
    pthread_mutex_unlock(&w->exports_lock);

    for (uint32_t i = 0; st == KVF_OK && i < w->identity.layers; ++i) {
        kvf_layer layer = layers[i];
        mi_block_id id = mi_page_ref_block(&ref, i);
        mi_block_meta meta;
        if (kvf_now_ms() >= deadline)
            st = KVF_TIMEOUT;
        else if (id != layer.id || (uint32_t)(id >> 32) != layer.generation)
            st = KVF_STALE;
        else
            st = mi_remote_from_storage(mi_storage_read_snapshot_event(w->storage, id, layer.version,
                                             dst, layer.size, stage ? stage->stream : NULL, stage ? stage->event : NULL, &meta));
        if (st == KVF_OK && kvf_crc32c(dst, layer.size) != layer.checksum)
            st = KVF_CRC;
        dst += layer.size;
    }
    mi_page_ref_release(&ref);
    return st;
}

kvf_status mi_remote_read_owned_page(mi_remote_worker *w, kvf_hash page,
                                     uint64_t deadline, void *data) {
    if (!w || !data || w->location != MI_LOCATION_CPU)
        return KVF_INVALID;
    return copy_batch_page(w, NULL, (kvf_page_key){w->space, page}, NULL, data, deadline);
}

static kvf_status send_batch(mi_remote_worker *w, mi_segment *stage, mi_batch_buffer *buffer, kvf_frame f,
                              const uint8_t *bytes, mi_remote_socket socket, uint64_t deadline) {
    kvf_reader reader = {.buffer = bytes, .size = f.size};
    kvf_batch_page pages[KVF_MAX_BATCH_PAGES];
    uint32_t count = kvf_get_batch(&reader, pages);
    size_t page_bytes = (size_t)w->identity.layers * w->identity.bytes_per_layer;
    size_t size = (size_t)count * page_bytes;
    kvf_status st = kvf_reader_done(&reader) && size <= KVF_MAX_BATCH_BYTES ? KVF_OK : KVF_INVALID;
    for (uint32_t i = 0; st == KVF_OK && i < count; ++i)
        if (!kvf_uuid_equal(pages[i].owner_boot, w->info.owner.boot) ||
            !kvf_hash_equal(pages[i].key.space, w->space))
            st = KVF_STALE;
    int control_fd = -1;
    if (st == KVF_OK)
        st = kvf_connect(w->control.endpoint, deadline, &control_fd);
    if (st == KVF_OK)
        st = kvf_client_validate_batch_on(control_fd, deadline, pages, count);
    if (st == KVF_OK && size > buffer->capacity) {
        void *data = NULL;
        st = mi_remote_from_storage(mi_device_host_alloc(w->device, size, &data));
        if (st == KVF_OK) {
            if (buffer->data)
                (void)mi_device_host_free(w->device, buffer->data);
            *buffer = (mi_batch_buffer){data, size};
        }
    }
    for (uint32_t i = 0; st == KVF_OK && i < count; ++i)
        st = copy_batch_page(w, stage, pages[i].key, &pages[i].manifest_digest, (unsigned char *)buffer->data + i * page_bytes, deadline);
    if (st == KVF_OK)
        st = kvf_client_validate_batch_on(control_fd, deadline, pages, count);
    if (control_fd >= 0)
        close(control_fd);
    if (st == KVF_OK) {
        pthread_mutex_lock(&w->exports_lock);
        for (uint32_t i = 0; i < count; ++i) {
            mi_remote_export *e = mi_remote_find_export(w, pages[i].key);
            if (!mi_remote_export_visible(e) || !kvf_hash_equal(e->digest, pages[i].manifest_digest)) {
                st = KVF_STALE;
                break;
            }
        }
        pthread_mutex_unlock(&w->exports_lock);
    }
    f.response = 1;
    f.status = st;
    f.size = st == KVF_OK ? (uint32_t)size : 0;
    return remote_send_frame(socket, f, buffer->data, deadline);
}

void mi_remote_serve(mi_remote_worker *w, unsigned lane, mi_remote_socket socket, uint64_t deadline) {
    uint8_t bytes[4 + KVF_MAX_BATCH_PAGES * 156];
    kvf_frame f;
    while (remote_recv_frame(socket, &f, bytes, sizeof(bytes), deadline, NULL) == KVF_OK && !f.response) {
        if (f.opcode < KVF_FETCH || f.opcode > KVF_FETCH_BATCH)
            return;
        mi_segment *stage = mi_segment_acquire(w->segments, lane, lane + 1);
        if (!stage)
            return;
        atomic_fetch_add_explicit(&w->active_transfers, 1, memory_order_relaxed);
        uint64_t end = kvf_now_ms() + w->control.timeout_ms;
        kvf_status st = f.opcode == KVF_FETCH_BATCH
            ? send_batch(w, stage, &w->batch_send[lane], f, bytes, socket, end)
            : send_layer(w, stage, f, bytes, sizeof(bytes), socket, end);
        atomic_fetch_sub_explicit(&w->active_transfers, 1, memory_order_relaxed);
        mi_segment_release(stage);
        if (st != KVF_OK || (f.opcode != KVF_FETCH_STREAM && f.opcode != KVF_FETCH_BATCH))
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

static uint64_t transfer_deadline(kvf_client client, uint64_t deadline) {
    uint64_t end = kvf_now_ms() + client.timeout_ms;
    return deadline && deadline < end ? deadline : end;
}

static int tcp_fallback(mi_remote_worker *w, kvf_transport mode, kvf_status st, uint64_t deadline) {
    if (mode != KVF_TRANSPORT_RDMA_RSOCKET || w->rdma_strict || kvf_now_ms() >= deadline ||
        (st != KVF_IO && st != KVF_TIMEOUT && st != KVF_UNAVAILABLE))
        return 0;
    if (!atomic_exchange_explicit(&w->warned_peer_fallback, 1, memory_order_relaxed))
        fprintf(stderr, "kv-fabric: warning: RDMA_RSOCKET peer transfer failed (%s). Retrying over TCP.\n",
                kvf_status_string(st));
    return 1;
}

static kvf_status borrow_peer(mi_remote_worker *w, const kvf_grant *g, kvf_transport mode, uint64_t end,
                              mi_peer_conn **peer, mi_remote_socket *socket, int *reused) {
    uint64_t connect_end = end;
    if (mode == KVF_TRANSPORT_RDMA_RSOCKET && !w->rdma_strict) {
        /* Leave room in the original request budget for a failed CM connection. */
        uint64_t now = kvf_now_ms();
        if (end > now)
            connect_end = now + (end - now) / 2;
    }
    kvf_status st = mi_peer_borrow(w->peers, g, mode, connect_end, peer, socket, reused);
    if (tcp_fallback(w, mode, st, end))
        st = mi_peer_borrow(w->peers, g, KVF_TRANSPORT_TCP, end, peer, socket, reused);
    return st;
}

kvf_status mi_remote_receive_batch(mi_remote_worker *w, const kvf_grant *grant, const kvf_batch_page *pages,
                                   uint32_t count, size_t size, uint64_t deadline_ms,
                                   const atomic_bool *cancel, mi_remote_timing *timing) {
    if (size > w->batch_recv.capacity) {
        uint64_t start = kvf_now_ns();
        void *data = realloc(w->batch_recv.data, size);
        timing->cpu_stage_ns += kvf_now_ns() - start;
        if (!data)
            return KVF_LIMIT;
        w->batch_recv = (mi_batch_buffer){data, size};
    }
    return mi_remote_receive_batch_into(w, grant, pages, count, size, deadline_ms, cancel,
                                        timing, w->batch_recv.data);
}

kvf_status mi_remote_receive_batch_into(mi_remote_worker *w, const kvf_grant *grant,
                                   const kvf_batch_page *pages, uint32_t count, size_t size,
                                   uint64_t deadline_ms, const atomic_bool *cancel,
                                   mi_remote_timing *timing, void *destination) {
    uint8_t bytes[4 + KVF_MAX_BATCH_PAGES * 156];
    kvf_writer request = {.buffer = bytes, .capacity = sizeof(bytes)};
    kvf_put_batch(&request, pages, count);
    uint64_t end = transfer_deadline(w->control, deadline_ms);
    kvf_transport mode = w->transport_mode;
    int reconnected = 0;
    for (;;) {
        if (mi_remote_cancelled(cancel))
            return KVF_CANCELLED;
        mi_remote_socket socket;
        int reused;
        mi_peer_conn *peer;
        uint64_t start = kvf_now_ns();
        kvf_status st = borrow_peer(w, grant, mode, end, &peer, &socket, &reused);
        timing->conn_acquire_ns += kvf_now_ns() - start;
        if (st != KVF_OK)
            return st;
        mode = socket.transport;
        kvf_receive_timing wire = {0};
        if (mi_remote_cancelled(cancel)) {
            st = KVF_CANCELLED;
        } else {
            ++timing->data_requests;
            st = remote_fetch(socket, KVF_FETCH_BATCH, &request, destination, size, end, &wire);
        }
        timing->net_recv_ns += wire.net_recv_ns;
        timing->reply_wait_ns += wire.reply_wait_ns;
        timing->received_bytes += wire.received_bytes;
        timing->crc_verify_ns += wire.crc_verify_ns;
        if (mi_remote_cancelled(cancel))
            st = KVF_CANCELLED;
        mi_peer_return(w->peers, peer, socket, st == KVF_OK);
        if (st == KVF_OK) {
            atomic_store_explicit(&w->active_transport, mode, memory_order_relaxed);
            return KVF_OK;
        }
        /* FIN can race a healthy idle check. A read-only request without a
         * received body can reconnect once, under the same deadline and lease. */
        if (wire.received_bytes)
            return st;
        if (st == KVF_IO && reused && !reconnected) {
            reconnected = 1;
            continue;
        }
        if (!tcp_fallback(w, mode, st, end))
            return st;
        mode = KVF_TRANSPORT_TCP;
    }
}

kvf_status mi_remote_import(mi_remote_worker *w, mi_kv_sequence *s, size_t page,
                             const kvf_grant *g, uint64_t deadline_ms, const atomic_bool *cancel) {
    kvf_fetch_request r = {
        .key = g->manifest.key, .owner_boot = g->manifest.owner.boot, .ticket = g->ticket
    };
    mi_segment *stage[2] = {0};
    mi_page_lease leases[2] = {0};
    mi_peer_conn *peer = NULL;
    mi_remote_socket socket = {.fd = -1};
    kvf_client client;
    kvf_status st = mi_remote_request_client(w->control, deadline_ms, cancel, &client);
    if (st != KVF_OK)
        return st;
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
        st = borrow_peer(w, g, w->transport_mode, transfer_deadline(client, deadline_ms), &peer, &socket, NULL);
        if (st != KVF_OK)
            goto done;
    }

    kvf_transport mode = peer ? socket.transport : w->transport_mode;
    uint64_t renew_at = kvf_now_ms() + g->ttl_ms / 2;
    for (uint32_t i = 0; i < g->manifest.layer_count; ++i) {
        st = mi_remote_request_client(w->control, deadline_ms, cancel, &client);
        if (st != KVF_OK)
            goto done;
        if (kvf_now_ms() >= renew_at) {
            uint32_t ttl_ms;
            st = kvf_client_renew(client, g->ticket, &ttl_ms);
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
        if (r.layer.size > lease->lease.size) {
            st = KVF_INVALID;
            goto done;
        }
        void *dst = stage[slot] ? stage[slot]->data : lease->lease.data;
        st = mi_remote_request_client(w->control, deadline_ms, cancel, &client);
        if (st != KVF_OK)
            goto done;
        uint64_t end = transfer_deadline(client, deadline_ms);
        if (!peer) {
            st = borrow_peer(w, g, mode, end, &peer, &socket, NULL);
            if (st != KVF_OK)
                goto done;
        }
        uint8_t bytes[156];
        kvf_writer request = {.buffer = bytes, .capacity = sizeof(bytes)};
        kvf_put_fetch(&request, &r);
        st = remote_fetch(socket, peer ? KVF_FETCH_STREAM : KVF_FETCH, &request, dst, r.layer.size, end, NULL);
        mode = socket.transport;
        if (tcp_fallback(w, mode, st, end)) {
            mi_peer_return(w->peers, peer, socket, 0);
            peer = NULL;
            socket.fd = -1;
            st = borrow_peer(w, g, KVF_TRANSPORT_TCP, end, &peer, &socket, NULL);
            if (st == KVF_OK)
                st = remote_fetch(socket, peer ? KVF_FETCH_STREAM : KVF_FETCH,
                                  &request, dst, r.layer.size, end, NULL);
            mode = socket.transport;
        }
        if (!peer) {
            remote_close(socket);
            socket.fd = -1;
        }
        if (st == KVF_OK && kvf_crc32c(dst, r.layer.size) != r.layer.checksum)
            st = KVF_CRC;
        if (st != KVF_OK)
            goto done;
        atomic_store_explicit(&w->active_transport, mode, memory_order_relaxed);
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
    st = mi_remote_request_client(w->control, deadline_ms, cancel, &client);
    if (st == KVF_OK)
        st = kvf_client_validate(client, &r);

done:
    for (unsigned i = 0; i < 2; ++i) {
        if (leases[i].kv)
            (void)mi_page_lease_abort(&leases[i]);
        if (stage[i] && mi_storage_quarantined(w->storage))
            mi_segment_quarantine(stage[i]);
        mi_segment_release(stage[i]);
    }
    mi_peer_return(w->peers, peer, socket, st == KVF_OK);
    return st;
}
