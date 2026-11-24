/* Field codec layer. Reader/writer borrow byte ranges. No allocation or locks. */
#include "kvfabric/wire.h"
#include <string.h>

static int fits(size_t capacity, size_t position, size_t size) {
    return position <= capacity && size <= capacity - position;
}

void kvf_put_bytes(kvf_writer *w, const void *data, size_t size) {
    if (w->failed || !fits(w->capacity, w->size, size) ||
        (size && (!w->buffer || !data))) {
        w->failed = 1;
        return;
    }
    if (size)
        memcpy(w->buffer + w->size, data, size);
    w->size += size;
}

void kvf_put_u16(kvf_writer *w, uint16_t n) {
    const uint8_t bytes[] = {(uint8_t)(n >> 8), (uint8_t)n};
    kvf_put_bytes(w, bytes, sizeof(bytes));
}

void kvf_put_u32(kvf_writer *w, uint32_t n) {
    const uint8_t bytes[] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    kvf_put_bytes(w, bytes, sizeof(bytes));
}

void kvf_put_u64(kvf_writer *w, uint64_t n) {
    kvf_put_u32(w, (uint32_t)(n >> 32));
    kvf_put_u32(w, (uint32_t)n);
}

void kvf_get_bytes(kvf_reader *r, void *data, size_t size) {
    if (r->failed || !fits(r->size, r->offset, size) ||
        (size && (!r->buffer || !data))) {
        r->failed = 1;
        return;
    }
    if (size)
        memcpy(data, r->buffer + r->offset, size);
    r->offset += size;
}

uint16_t kvf_get_u16(kvf_reader *r) {
    uint8_t b[2] = {0};
    kvf_get_bytes(r, b, sizeof(b));
    return (uint16_t)((uint16_t)b[0] << 8 | b[1]);
}

uint32_t kvf_get_u32(kvf_reader *r) {
    uint8_t b[4] = {0};
    kvf_get_bytes(r, b, sizeof(b));
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

uint64_t kvf_get_u64(kvf_reader *r) {
    uint64_t high = kvf_get_u32(r);
    return high << 32 | kvf_get_u32(r);
}

int kvf_reader_done(const kvf_reader *r) {
    return !r->failed && r->size == r->offset;
}

void kvf_put_identity(kvf_writer *w, const kvf_identity *i) {
    kvf_put_bytes(w, i->model.bytes, 32);
    kvf_put_bytes(w, i->tokenizer.bytes, 32);
    kvf_put_bytes(w, i->layout.bytes, 32);
    kvf_put_bytes(w, i->lora.bytes, 32);
    kvf_put_bytes(w, i->tenant.bytes, 32);
    kvf_put_u32(w, i->tokens_per_page);
    kvf_put_u32(w, i->layers);
    kvf_put_u32(w, i->bytes_per_layer);
    kvf_put_u16(w, i->dtype);
    kvf_put_u16(w, i->tp_rank);
    kvf_put_u16(w, i->tp_size);
    kvf_put_u16(w, i->pp_rank);
    kvf_put_u16(w, i->pp_size);
}

static void get_identity(kvf_reader *r, kvf_identity *i) {
    kvf_get_bytes(r, i->model.bytes, 32);
    kvf_get_bytes(r, i->tokenizer.bytes, 32);
    kvf_get_bytes(r, i->layout.bytes, 32);
    kvf_get_bytes(r, i->lora.bytes, 32);
    kvf_get_bytes(r, i->tenant.bytes, 32);
    i->tokens_per_page = kvf_get_u32(r);
    i->layers = kvf_get_u32(r);
    i->bytes_per_layer = kvf_get_u32(r);
    i->dtype = kvf_get_u16(r);
    i->tp_rank = kvf_get_u16(r);
    i->tp_size = kvf_get_u16(r);
    i->pp_rank = kvf_get_u16(r);
    i->pp_size = kvf_get_u16(r);
}

void kvf_put_owner(kvf_writer *w, const kvf_owner *owner) {
    if (!kvf_owner_valid(owner)) {
        w->failed = 1;
        return;
    }
    size_t size = strlen(owner->name);
    kvf_put_u16(w, (uint16_t)size);
    kvf_put_bytes(w, owner->name, size);
    kvf_put_bytes(w, owner->boot.bytes, 16);
}

void kvf_get_owner(kvf_reader *r, kvf_owner *owner) {
    *owner = (kvf_owner){0};
    uint16_t size = kvf_get_u16(r);
    if (!size || size >= sizeof(owner->name)) {
        r->failed = 1;
        return;
    }
    kvf_get_bytes(r, owner->name, size);
    kvf_get_bytes(r, owner->boot.bytes, 16);
    if (memchr(owner->name, '\0', size) || !kvf_owner_valid(owner))
        r->failed = 1;
}

void kvf_put_endpoint(kvf_writer *w, kvf_endpoint ep) {
    kvf_put_bytes(w, ep.ipv4, 4);
    kvf_put_u16(w, ep.port);
}

void kvf_get_endpoint(kvf_reader *r, kvf_endpoint *ep) {
    kvf_get_bytes(r, ep->ipv4, 4);
    ep->port = kvf_get_u16(r);
}

void kvf_put_worker(kvf_writer *w, const kvf_worker_info *worker) {
    kvf_put_owner(w, &worker->owner);
    kvf_put_endpoint(w, worker->endpoint);
    kvf_put_u32(w, worker->capacity_pages);
    kvf_put_u32(w, worker->published_pages);
    kvf_put_u32(w, worker->active_transfers);
    kvf_put_u64(w, worker->local_hits);
    kvf_put_u64(w, worker->remote_hits);
    kvf_put_u64(w, worker->reused_tokens);
    kvf_put_u64(w, worker->failed_fetches);
}

void kvf_get_worker(kvf_reader *r, kvf_worker_info *worker) {
    kvf_get_owner(r, &worker->owner);
    kvf_get_endpoint(r, &worker->endpoint);
    worker->capacity_pages = kvf_get_u32(r);
    worker->published_pages = kvf_get_u32(r);
    worker->active_transfers = kvf_get_u32(r);
    worker->local_hits = kvf_get_u64(r);
    worker->remote_hits = kvf_get_u64(r);
    worker->reused_tokens = kvf_get_u64(r);
    worker->failed_fetches = kvf_get_u64(r);
}

void kvf_put_key(kvf_writer *w, kvf_page_key key) {
    kvf_put_bytes(w, key.space.bytes, 32);
    kvf_put_bytes(w, key.page.bytes, 32);
}

void kvf_get_key(kvf_reader *r, kvf_page_key *key) {
    kvf_get_bytes(r, key->space.bytes, 32);
    kvf_get_bytes(r, key->page.bytes, 32);
}

void kvf_put_layer(kvf_writer *w, kvf_layer layer) {
    kvf_put_u64(w, layer.id);
    kvf_put_u64(w, layer.version);
    kvf_put_u32(w, layer.generation);
    kvf_put_u32(w, layer.size);
    kvf_put_u32(w, layer.checksum);
}

void kvf_get_layer(kvf_reader *r, kvf_layer *layer) {
    layer->id = kvf_get_u64(r);
    layer->version = kvf_get_u64(r);
    layer->generation = kvf_get_u32(r);
    layer->size = kvf_get_u32(r);
    layer->checksum = kvf_get_u32(r);
}

void kvf_put_manifest(kvf_writer *w, const kvf_manifest *m) {
    if (m->layer_count > KVF_MAX_LAYERS) {
        w->failed = 1;
        return;
    }
    kvf_put_key(w, m->key);
    kvf_put_bytes(w, m->parent.bytes, 32);
    kvf_put_owner(w, &m->owner);
    kvf_put_identity(w, &m->identity);
    kvf_put_u32(w, m->token_count);
    kvf_put_u32(w, m->layer_count);
    for (uint32_t i = 0; i < m->layer_count; ++i)
        kvf_put_layer(w, m->layer[i]);
}

void kvf_get_manifest(kvf_reader *r, kvf_manifest *m) {
    *m = (kvf_manifest){0};
    kvf_get_key(r, &m->key);
    kvf_get_bytes(r, m->parent.bytes, 32);
    kvf_get_owner(r, &m->owner);
    get_identity(r, &m->identity);
    m->token_count = kvf_get_u32(r);
    m->layer_count = kvf_get_u32(r);
    if (m->layer_count > KVF_MAX_LAYERS) {
        r->failed = 1;
        return;
    }
    for (uint32_t i = 0; i < m->layer_count; ++i)
        kvf_get_layer(r, &m->layer[i]);
}

void kvf_put_ticket(kvf_writer *w, kvf_ticket t) {
    kvf_put_bytes(w, t.directory.bytes, 16);
    kvf_put_bytes(w, t.secret.bytes, 16);
    kvf_put_u64(w, t.fence);
    kvf_put_u32(w, t.slot);
}

void kvf_get_ticket(kvf_reader *r, kvf_ticket *t) {
    kvf_get_bytes(r, t->directory.bytes, 16);
    kvf_get_bytes(r, t->secret.bytes, 16);
    t->fence = kvf_get_u64(r);
    t->slot = kvf_get_u32(r);
}

void kvf_put_fetch(kvf_writer *w, const kvf_fetch_request *req) {
    kvf_put_key(w, req->key);
    kvf_put_bytes(w, req->owner_boot.bytes, 16);
    kvf_put_ticket(w, req->ticket);
    kvf_put_u32(w, req->layer_index);
    kvf_put_layer(w, req->layer);
}

void kvf_get_fetch(kvf_reader *r, kvf_fetch_request *req) {
    kvf_get_key(r, &req->key);
    kvf_get_bytes(r, req->owner_boot.bytes, 16);
    kvf_get_ticket(r, &req->ticket);
    req->layer_index = kvf_get_u32(r);
    kvf_get_layer(r, &req->layer);
}

kvf_status kvf_manifest_digest(const kvf_manifest *manifest, kvf_hash *out) {
    uint8_t bytes[KVF_METADATA_BYTES];
    kvf_writer w = {.buffer = bytes, .capacity = sizeof(bytes)};
    kvf_put_manifest(&w, manifest);
    return w.failed ? KVF_INVALID : kvf_hash_bytes(bytes, w.size, out);
}

void kvf_put_batch(kvf_writer *w, const kvf_batch_page *pages, uint32_t count) {
    if (!pages || !count || count > KVF_MAX_BATCH_PAGES) {
        w->failed = 1;
        return;
    }
    kvf_put_u32(w, count);
    for (uint32_t i = 0; i < count; ++i) {
        kvf_put_key(w, pages[i].key);
        kvf_put_bytes(w, pages[i].owner_boot.bytes, 16);
        kvf_put_ticket(w, pages[i].ticket);
        kvf_put_bytes(w, pages[i].manifest_digest.bytes, 32);
    }
}

uint32_t kvf_get_batch(kvf_reader *r, kvf_batch_page pages[KVF_MAX_BATCH_PAGES]) {
    uint32_t count = kvf_get_u32(r);
    if (!count || count > KVF_MAX_BATCH_PAGES) {
        r->failed = 1;
        return 0;
    }
    for (uint32_t i = 0; i < count; ++i) {
        kvf_get_key(r, &pages[i].key);
        kvf_get_bytes(r, pages[i].owner_boot.bytes, 16);
        kvf_get_ticket(r, &pages[i].ticket);
        kvf_get_bytes(r, pages[i].manifest_digest.bytes, 32);
    }
    return count;
}
