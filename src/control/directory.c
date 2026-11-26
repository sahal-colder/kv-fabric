/*
 * L2-L4 control plane. Directory owns all registry, replica and lease arrays.
 * lock covers every mutation and read snapshot. No calls to sockets or storage under it.
 */
#define _POSIX_C_SOURCE 200809L
#include "kvfabric/directory.h"
#include "kvfabric/wire.h"
#include <openssl/crypto.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    kvf_worker_info info;   /* published_pages counts this boot's linked replicas. */
    uint64_t deadline;      /* Monotonic worker expiry. Zero marks a retired boot. */
} worker_entry;

typedef struct {
    kvf_manifest manifest;  /* Immutable until the replica is unlinked. */
    kvf_hash digest;         /* Canonical manifest for the batch data path. */
    uint64_t revision;      /* Nonzero for a linked replica. A lease binds this incarnation. */
    uint64_t touched;       /* Logical clock for LRU, advanced on use or first publish. */
    uint32_t worker;        /* Registry slot matching manifest.owner. */
    uint32_t next;          /* Next bucket record, or next free slot when unlinked. */
    unsigned protected;     /* Computed during eviction under the Directory lock. */
} page_entry;

typedef struct {
    kvf_ticket ticket;      /* Epoch, fence and random secret identify one use of this slot. */
    uint64_t deadline;     /* Monotonic expiry independent of worker TTL. */
    uint64_t page_revision; /* Snapshot prevents reuse of a manifest slot from reviving the lease. */
    uint32_t page;         /* Replica slot bound by the grant. */
} lease_entry;

struct kvf_directory {
    pthread_mutex_t lock;       /* Protects tables and counters below. */
    kvf_directory_config config; /* Immutable bounds and TTLs. */
    kvf_directory_stats stats;  /* Counts of live metadata and completed operations. */
    kvf_uuid epoch;             /* Fresh on every startup. */
    uint64_t next_revision;     /* Increases whenever a replica slot is linked. */
    uint64_t next_fence;         /* Increases whenever a lease is granted. */
    uint64_t clock;              /* Logical LRU order independent of wall time. */
    uint32_t workers_used;      /* Allocated Worker slots, including retired boots. */
    uint32_t free_page;         /* Free-list head, zero when full. */
    uint32_t next_lease;        /* Rotating lease allocator cursor. */
    uint32_t mask;               /* Bounds the power-of-two replica hash table. */
    worker_entry *workers;      /* Owned, indexed by page_entry.worker. */
    page_entry *pages;          /* Owned, index zero is the sentinel. */
    lease_entry *leases;        /* Owned, indexed by ticket.slot. */
    uint32_t *buckets;          /* Owns one head per replica hash bucket. */
};

void kvf_directory_defaults(kvf_directory_config *config) {
    *config = (kvf_directory_config){
        .max_pages = 4096, .max_workers = 128, .max_leases = 4096,
        .worker_ttl_ms = 8000, .lease_ttl_ms = 15000
    };
}

kvf_status kvf_directory_create(kvf_directory_config c, kvf_directory **out) {
    if (!out || !c.max_pages || c.max_pages > 65536 ||
        !c.max_workers || c.max_workers > 1024 || !c.max_leases || c.max_leases > 65536 ||
        !c.worker_ttl_ms || c.worker_ttl_ms > 600000 || !c.lease_ttl_ms || c.lease_ttl_ms > 600000)
        return KVF_INVALID;

    kvf_directory *d = calloc(1, sizeof(*d));
    if (!d)
        return KVF_LIMIT;
    d->config = c;
    uint32_t buckets = 2;
    while (buckets / 2 < c.max_pages)
        buckets *= 2;
    d->mask = buckets - 1;
    d->buckets = calloc(buckets, sizeof(*d->buckets));
    d->pages = calloc(c.max_pages + 1u, sizeof(*d->pages));
    d->workers = calloc(c.max_workers, sizeof(*d->workers));
    d->leases = calloc(c.max_leases, sizeof(*d->leases));
    kvf_status st = KVF_LIMIT;
    if (!d->buckets || !d->pages || !d->workers || !d->leases)
        goto fail;
    st = kvf_uuid_new(&d->epoch);
    if (st != KVF_OK)
        goto fail;
    st = KVF_INTERNAL;
    if (pthread_mutex_init(&d->lock, NULL) != 0)
        goto fail;
    for (uint32_t i = 1; i < c.max_pages; ++i)
        d->pages[i].next = i + 1;
    d->free_page = 1;
    *out = d;
    return KVF_OK;

fail:
    free(d->leases);
    free(d->workers);
    free(d->pages);
    free(d->buckets);
    free(d);
    return st;
}

void kvf_directory_destroy(kvf_directory *d) {
    if (!d)
        return;
    pthread_mutex_destroy(&d->lock);
    free(d->leases);
    free(d->workers);
    free(d->pages);
    free(d->buckets);
    free(d);
}

static worker_entry *find_worker(kvf_directory *d, const char *name) {
    for (uint32_t i = 0; i < d->workers_used; ++i)
        if (strcmp(d->workers[i].info.owner.name, name) == 0)
            return &d->workers[i];
    return NULL;
}

static uint32_t *page_link(kvf_directory *d, kvf_page_key key, uint32_t worker) {
    const uint8_t *b = key.page.bytes;
    uint32_t hash = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    uint32_t *link = &d->buckets[hash & d->mask];
    while (*link && (!kvf_page_key_equal(d->pages[*link].manifest.key, key) ||
                     (worker != UINT32_MAX && worker != d->pages[*link].worker)))
        link = &d->pages[*link].next;
    return link;
}

static void remove_page(kvf_directory *d, uint32_t *link) {
    uint32_t slot = *link;
    page_entry *p = &d->pages[slot];
    *link = p->next;
    --d->workers[p->worker].info.published_pages;
    p->revision = 0;
    p->next = d->free_page;
    d->free_page = slot;
    --d->stats.pages;
}

static void retire_worker(kvf_directory *d, worker_entry *w) {
    w->deadline = 0;
    for (uint32_t i = 1; w->info.published_pages && i <= d->config.max_pages; ++i) {
        page_entry *p = &d->pages[i];
        if (p->revision && &d->workers[p->worker] == w)
            remove_page(d, page_link(d, p->manifest.key, p->worker));
    }
}

static void expire_workers(kvf_directory *d, uint64_t now) {
    for (uint32_t i = 0; i < d->workers_used; ++i) {
        worker_entry *w = &d->workers[i];
        if (w->deadline && w->deadline <= now)
            retire_worker(d, w);
    }
}

/* Keep an offline boot for status lookups until its slot is needed again. */
static worker_entry *reserve_worker(kvf_directory *d, uint64_t now) {
    if (d->workers_used < d->config.max_workers)
        return &d->workers[d->workers_used++];
    expire_workers(d, now);
    for (uint32_t i = 0; i < d->workers_used; ++i)
        if (!d->workers[i].deadline)
            return &d->workers[i];
    return NULL;
}

static int worker_valid(const kvf_worker_info *info) {
    return info && kvf_owner_valid(&info->owner) && info->capacity_pages && info->endpoint.port &&
        (info->endpoint.ipv4[0] || info->endpoint.ipv4[1] || info->endpoint.ipv4[2] || info->endpoint.ipv4[3]);
}

static int worker_matches(const worker_entry *w, const kvf_worker_info *info) {
    return w->info.endpoint.port == info->endpoint.port &&
        memcmp(w->info.endpoint.ipv4, info->endpoint.ipv4, 4) == 0 &&
        w->info.capacity_pages == info->capacity_pages;
}

static void update_worker(kvf_directory *d, worker_entry *w, const kvf_worker_info *info, uint64_t now) {
    uint32_t published = w->info.published_pages;
    w->info = *info;
    w->info.published_pages = published;
    w->deadline = now + d->config.worker_ttl_ms;
}

kvf_status kvf_directory_register(kvf_directory *d, const kvf_worker_info *info, kvf_uuid replaces) {
    if (!d)
        return KVF_INVALID;
    if (!worker_valid(info))
        return KVF_INVALID;
    pthread_mutex_lock(&d->lock);
    uint64_t now = kvf_now_ms();
    worker_entry *w = find_worker(d, info->owner.name);
    kvf_status st = KVF_OK;
    if (!w) {
        if (!kvf_uuid_zero(replaces))
            st = KVF_STALE;
        else if (!(w = reserve_worker(d, now)))
            st = KVF_LIMIT;
    } else if (kvf_uuid_equal(w->info.owner.boot, info->owner.boot)) {
        if (!worker_matches(w, info))
            st = KVF_CONFLICT;
        else if (w->deadline <= now)
            retire_worker(d, w);
    } else if (kvf_uuid_equal(w->info.owner.boot, replaces)) {
        retire_worker(d, w);
    } else {
        st = KVF_STALE;
    }
    if (st == KVF_OK)
        update_worker(d, w, info, now);
    pthread_mutex_unlock(&d->lock);
    return st;
}

kvf_status kvf_directory_heartbeat(kvf_directory *d, const kvf_worker_info *info) {
    if (!d)
        return KVF_INVALID;
    if (!worker_valid(info))
        return KVF_INVALID;
    pthread_mutex_lock(&d->lock);
    uint64_t now = kvf_now_ms();
    worker_entry *w = find_worker(d, info->owner.name);
    kvf_status st = KVF_NOT_FOUND;
    if (w) {
        if (!kvf_uuid_equal(w->info.owner.boot, info->owner.boot)) {
            st = KVF_STALE;
        } else if (w->deadline <= now) {
            retire_worker(d, w);
        } else if (!worker_matches(w, info)) {
            st = KVF_CONFLICT;
        } else {
            update_worker(d, w, info, now);
            st = KVF_OK;
        }
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}

kvf_status kvf_directory_unregister(kvf_directory *d, const kvf_owner *owner) {
    if (!d)
        return KVF_INVALID;
    if (!kvf_owner_valid(owner))
        return KVF_INVALID;
    pthread_mutex_lock(&d->lock);
    worker_entry *w = find_worker(d, owner->name);
    kvf_status st = !w ? KVF_NOT_FOUND :
        kvf_uuid_equal(w->info.owner.boot, owner->boot) ? KVF_OK : KVF_STALE;
    if (st == KVF_OK)
        retire_worker(d, w);
    pthread_mutex_unlock(&d->lock);
    return st;
}

kvf_status kvf_directory_worker_status(kvf_directory *d, const char *name, kvf_worker_info *out, int *online) {
    if (!d || !name || !out || !online || !name[0] || strnlen(name, KVF_WORKER_NAME) == KVF_WORKER_NAME)
        return KVF_INVALID;
    pthread_mutex_lock(&d->lock);
    uint64_t now = kvf_now_ms();
    worker_entry *w = find_worker(d, name);
    if (w) {
        *online = w->deadline > now;
        if (!*online)
            retire_worker(d, w);
        *out = w->info;
    }
    pthread_mutex_unlock(&d->lock);
    return w ? KVF_OK : KVF_NOT_FOUND;
}

static int same_content(const kvf_manifest *a, const kvf_manifest *b) {
    /* A new physical revision must retire the old manifest before publishing. */
    if (!kvf_owner_equal(&a->owner, &b->owner) || a->layer_count != b->layer_count ||
        !kvf_hash_equal(a->parent, b->parent))
        return 0;
    for (uint32_t i = 0; i < a->layer_count; ++i)
        if (!kvf_layer_equal(a->layer[i], b->layer[i]))
            return 0;
    return 1;
}

static int lease_live(const kvf_directory *d, const lease_entry *l, uint64_t now) {
    if (l->deadline <= now || !l->page_revision)
        return 0;
    const page_entry *p = &d->pages[l->page];
    return p->revision == l->page_revision && d->workers[p->worker].deadline > now;
}

static void protect_key(kvf_directory *d, kvf_page_key key) {
    if (kvf_hash_zero(key.page))
        return;
    for (uint32_t i = *page_link(d, key, UINT32_MAX); i; i = d->pages[i].next)
        if (kvf_page_key_equal(d->pages[i].manifest.key, key))
            d->pages[i].protected = 1;
}

static void reclaim_leaf(kvf_directory *d, const kvf_manifest *incoming, uint64_t now) {
    for (uint32_t i = 1; i <= d->config.max_pages; ++i)
        d->pages[i].protected = 0;
    for (uint32_t i = 1; i <= d->config.max_pages; ++i) {
        const page_entry *p = &d->pages[i];
        if (p->revision)
            protect_key(d, (kvf_page_key){p->manifest.key.space, p->manifest.parent});
    }
    /* The new leaf must not evict its own prefix while making room. */
    protect_key(d, (kvf_page_key){incoming->key.space, incoming->parent});
    for (uint32_t i = 0; i < d->config.max_leases; ++i)
        if (lease_live(d, &d->leases[i], now))
            d->pages[d->leases[i].page].protected = 1;

    page_entry *oldest = NULL;
    for (uint32_t i = 1; i <= d->config.max_pages; ++i) {
        page_entry *p = &d->pages[i];
        if (p->revision && !p->protected && (!oldest || p->touched < oldest->touched))
            oldest = p;
    }
    if (oldest) {
        remove_page(d, page_link(d, oldest->manifest.key, oldest->worker));
        ++d->stats.invalidations;
    }
}

kvf_status kvf_directory_publish(kvf_directory *d, const kvf_manifest *m) {
    if (!d)
        return KVF_INVALID;
    kvf_status st = kvf_manifest_validate(m);
    if (st != KVF_OK)
        return st;
    kvf_hash digest;
    st = kvf_manifest_digest(m, &digest);
    if (st != KVF_OK)
        return st;
    pthread_mutex_lock(&d->lock);
    uint64_t now = kvf_now_ms();
    worker_entry *w = find_worker(d, m->owner.name);
    if (!w || w->deadline <= now || !kvf_uuid_equal(w->info.owner.boot, m->owner.boot)) {
        st = KVF_STALE;
        goto done;
    }
    uint32_t worker = (uint32_t)(w - d->workers);
    uint32_t *link = page_link(d, m->key, worker);
    if (*link) {
        st = same_content(&d->pages[*link].manifest, m) ? KVF_OK : KVF_CONFLICT;
        if (st == KVF_OK)
            ++d->stats.duplicates;
        goto done;
    }
    for (uint32_t i = *page_link(d, m->key, UINT32_MAX); i; i = d->pages[i].next) {
        const page_entry *p = &d->pages[i];
        if (kvf_page_key_equal(p->manifest.key, m->key) && d->workers[p->worker].deadline > now) {
            if (!kvf_hash_equal(p->manifest.parent, m->parent)) {
                st = KVF_CONFLICT;
                goto done;
            }
            /* All replicas of a key must serve identical tensor bytes. */
            for (uint32_t layer = 0; layer < m->layer_count; ++layer) {
                if (p->manifest.layer[layer].checksum != m->layer[layer].checksum) {
                    st = KVF_CONFLICT;
                    goto done;
                }
            }
            break;
        }
    }
    st = KVF_LIMIT;
    if (w->info.published_pages >= w->info.capacity_pages || d->next_revision == UINT64_MAX)
        goto done;
    if (!d->free_page) {
        expire_workers(d, now);
        if (!d->free_page)
            reclaim_leaf(d, m, now);
        if (!d->free_page)
            goto done;
        link = page_link(d, m->key, worker);
    }

    uint32_t slot = d->free_page;
    page_entry *p = &d->pages[slot];
    d->free_page = p->next;
    *p = (page_entry){.manifest = *m, .digest = digest, .revision = ++d->next_revision,
                      .touched = ++d->clock, .worker = worker};
    *link = slot;
    ++w->info.published_pages;
    ++d->stats.pages;
    ++d->stats.publishes;
    st = KVF_OK;

done:
    pthread_mutex_unlock(&d->lock);
    return st;
}

static uint32_t select_replica(kvf_directory *d, kvf_hash space, const kvf_hash *pages,
                               uint32_t count, uint64_t now) {
    kvf_page_key first = {space, pages[0]};
    uint32_t best = 0, longest = 0, least_load = UINT32_MAX;
    for (uint32_t i = *page_link(d, first, UINT32_MAX); i; i = d->pages[i].next) {
        const page_entry *p = &d->pages[i];
        const worker_entry *w = &d->workers[p->worker];
        if (!kvf_page_key_equal(p->manifest.key, first) || w->deadline <= now)
            continue;
        uint32_t n = 1;
        for (; n < count; ++n) {
            uint32_t next = *page_link(d, (kvf_page_key){space, pages[n]}, p->worker);
            if (!next || !kvf_hash_equal(d->pages[next].manifest.parent, pages[n - 1]))
                break;
        }
        if (n > longest || (n == longest && w->info.active_transfers < least_load)) {
            best = i;
            longest = n;
            least_load = w->info.active_transfers;
        }
    }
    return best;
}

static int valid_lookup(kvf_hash space, const kvf_hash *pages, uint32_t count) {
    return pages && count && count <= KVF_MAX_LOOKUP && !kvf_hash_zero(space);
}

kvf_status kvf_directory_lookup_prefix(kvf_directory *d, kvf_hash space, const kvf_hash *pages,
                                        uint32_t count, uint32_t *matched) {
    if (!d)
        return KVF_INVALID;
    if (!matched || !valid_lookup(space, pages, count))
        return KVF_INVALID;
    pthread_mutex_lock(&d->lock);
    uint64_t now = kvf_now_ms();
    uint32_t n = 0;
    for (; n < count; ++n) {
        uint32_t page = select_replica(d, space, pages + n, 1, now);
        if (!page || (n && !kvf_hash_equal(d->pages[page].manifest.parent, pages[n - 1])))
            break;
        d->pages[page].touched = ++d->clock;
    }
    ++d->stats.lookups;
    d->stats.matched_pages += n;
    pthread_mutex_unlock(&d->lock);
    *matched = n;
    return KVF_OK;
}

kvf_status kvf_directory_lookup_page(kvf_directory *d, kvf_page_key key, kvf_manifest *m,
                                      kvf_endpoint *ep) {
    if (!d || !m || !ep)
        return KVF_INVALID;
    pthread_mutex_lock(&d->lock);
    uint32_t slot = select_replica(d, key.space, &key.page, 1, kvf_now_ms());
    if (slot) {
        page_entry *p = &d->pages[slot];
        p->touched = ++d->clock;
        *m = p->manifest;
        *ep = d->workers[p->worker].info.endpoint;
    }
    pthread_mutex_unlock(&d->lock);
    return slot ? KVF_OK : KVF_NOT_FOUND;
}

static kvf_status grant_page(kvf_directory *d, uint32_t page, uint64_t now, kvf_uuid secret, kvf_grant *out) {
    if (!page)
        return KVF_NOT_FOUND;
    if (d->next_fence == UINT64_MAX)
        return KVF_LIMIT;
    for (uint32_t probe = 0; probe < d->config.max_leases; ++probe) {
        uint32_t slot = d->next_lease;
        d->next_lease = (slot + 1) % d->config.max_leases;
        lease_entry *l = &d->leases[slot];
        if (lease_live(d, l, now))
            continue;
        page_entry *p = &d->pages[page];
        kvf_ticket t = {.directory = d->epoch, .secret = secret, .fence = ++d->next_fence, .slot = slot};
        *l = (lease_entry){.ticket = t, .deadline = now + d->config.lease_ttl_ms,
                           .page_revision = p->revision, .page = page};
        p->touched = ++d->clock;
        *out = (kvf_grant){.manifest = p->manifest, .endpoint = d->workers[p->worker].info.endpoint,
                           .ticket = t, .ttl_ms = d->config.lease_ttl_ms};
        return KVF_OK;
    }
    return KVF_LIMIT;
}

kvf_status kvf_directory_acquire_prefix(kvf_directory *d, kvf_hash space, const kvf_hash *pages,
                                        uint32_t count, kvf_grant *out) {
    if (!d)
        return KVF_INVALID;
    if (!out || !valid_lookup(space, pages, count))
        return KVF_INVALID;
    kvf_uuid secret;
    kvf_status st = kvf_uuid_new(&secret);
    if (st != KVF_OK)
        return st;
    pthread_mutex_lock(&d->lock);
    uint64_t now = kvf_now_ms();
    st = grant_page(d, select_replica(d, space, pages, count, now), now, secret, out);
    pthread_mutex_unlock(&d->lock);
    return st;
}

kvf_status kvf_directory_acquire_batch(kvf_directory *d, kvf_hash space, const kvf_hash *pages,
                                       uint32_t count, kvf_grant *out, uint32_t *matched) {
    if (!d)
        return KVF_INVALID;
    if (!pages || !out || !matched || !count || count > KVF_MAX_BATCH_PAGES || kvf_hash_zero(space))
        return KVF_INVALID;
    kvf_uuid secrets[KVF_MAX_BATCH_PAGES];
    for (uint32_t i = 0; i < count; ++i) {
        kvf_status st = kvf_uuid_new(&secrets[i]);
        if (st != KVF_OK)
            return st;
    }
    pthread_mutex_lock(&d->lock);
    uint64_t now = kvf_now_ms();
    uint32_t n = 0, worker = UINT32_MAX;
    kvf_status st = KVF_OK;
    while (n < count) {
        uint32_t page = worker == UINT32_MAX ? 0 : *page_link(d, (kvf_page_key){space, pages[n]}, worker);
        if (!page)
            page = select_replica(d, space, pages + n, count - n, now);
        if (page && n && !kvf_hash_equal(d->pages[page].manifest.parent, pages[n - 1]))
            page = 0;
        st = grant_page(d, page, now, secrets[n], &out[n]);
        if (st != KVF_OK)
            break;
        worker = d->pages[page].worker;
        ++n;
    }
    ++d->stats.lookups;
    d->stats.matched_pages += n;
    pthread_mutex_unlock(&d->lock);
    *matched = n;
    return n ? KVF_OK : st;
}

kvf_status kvf_directory_acquire(kvf_directory *d, kvf_page_key key, kvf_grant *out) {
    if (!d)
        return KVF_INVALID;
    return kvf_directory_acquire_prefix(d, key.space, &key.page, 1, out);
}

static lease_entry *find_lease(kvf_directory *d, kvf_ticket t) {
    if (!t.fence || t.slot >= d->config.max_leases || !kvf_uuid_equal(d->epoch, t.directory))
        return NULL;
    lease_entry *l = &d->leases[t.slot];
    return l->ticket.fence == t.fence && CRYPTO_memcmp(l->ticket.secret.bytes, t.secret.bytes, 16) == 0
        ? l : NULL;
}

kvf_status kvf_directory_renew(kvf_directory *d, kvf_ticket t, uint32_t *ttl) {
    if (!d || !ttl)
        return KVF_INVALID;
    pthread_mutex_lock(&d->lock);
    lease_entry *l = find_lease(d, t);
    uint64_t now = kvf_now_ms();
    kvf_status st = KVF_STALE;
    if (l && lease_live(d, l, now)) {
        l->deadline = now + d->config.lease_ttl_ms;
        *ttl = d->config.lease_ttl_ms;
        st = KVF_OK;
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}

kvf_status kvf_directory_release(kvf_directory *d, kvf_ticket t) {
    if (!d)
        return KVF_INVALID;
    pthread_mutex_lock(&d->lock);
    lease_entry *l = find_lease(d, t);
    kvf_status st = KVF_STALE;
    if (l && lease_live(d, l, kvf_now_ms())) {
        l->deadline = 0;
        st = KVF_OK;
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}

kvf_status kvf_directory_validate(kvf_directory *d, const kvf_fetch_request *r) {
    if (!d)
        return KVF_INVALID;
    if (!r || r->layer_index >= KVF_MAX_LAYERS)
        return KVF_INVALID;
    pthread_mutex_lock(&d->lock);
    lease_entry *l = find_lease(d, r->ticket);
    kvf_status st = KVF_STALE;
    if (l && lease_live(d, l, kvf_now_ms())) {
        const kvf_manifest *m = &d->pages[l->page].manifest;
        if (kvf_page_key_equal(m->key, r->key) && kvf_uuid_equal(m->owner.boot, r->owner_boot) &&
            r->layer_index < m->layer_count && kvf_layer_equal(m->layer[r->layer_index], r->layer))
            st = KVF_OK;
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}

kvf_status kvf_directory_check_batch(kvf_directory *d, const kvf_batch_page *pages, uint32_t count, int finish) {
    if (!d)
        return KVF_INVALID;
    if (!pages || !count || count > KVF_MAX_BATCH_PAGES)
        return KVF_INVALID;
    pthread_mutex_lock(&d->lock);
    uint64_t now = kvf_now_ms();
    kvf_status st = KVF_OK;
    for (uint32_t i = 0; i < count; ++i) {
        const kvf_batch_page *r = &pages[i];
        lease_entry *lease = find_lease(d, r->ticket);
        if (!lease || !lease_live(d, lease, now)) {
            st = KVF_STALE;
            continue;
        }
        const page_entry *p = &d->pages[lease->page];
        if (!kvf_page_key_equal(p->manifest.key, r->key) ||
            !kvf_uuid_equal(p->manifest.owner.boot, r->owner_boot) ||
            !kvf_hash_equal(p->digest, r->manifest_digest))
            st = KVF_STALE;
        if (finish)
            lease->deadline = 0;
    }
    pthread_mutex_unlock(&d->lock);
    return st;
}

static kvf_status invalidate(kvf_directory *d, const kvf_manifest *m, int automatic) {
    kvf_status st = kvf_manifest_validate(m);
    if (st != KVF_OK)
        return st;
    pthread_mutex_lock(&d->lock);
    worker_entry *worker = find_worker(d, m->owner.name);
    uint32_t *link = worker ? page_link(d, m->key, (uint32_t)(worker - d->workers)) : NULL;
    st = KVF_NOT_FOUND;
    if (!link || !*link)
        goto done;
    st = KVF_STALE;
    if (!same_content(&d->pages[*link].manifest, m))
        goto done;
    if (automatic) {
        uint64_t now = kvf_now_ms();
        st = KVF_CONFLICT;
        for (uint32_t i = 0; i < d->config.max_leases; ++i)
            if (d->leases[i].page == *link && lease_live(d, &d->leases[i], now))
                goto done;
        int other_replica = 0;
        for (uint32_t i = *page_link(d, m->key, UINT32_MAX); i; i = d->pages[i].next) {
            const page_entry *p = &d->pages[i];
            if (i != *link && kvf_page_key_equal(p->manifest.key, m->key) &&
                d->workers[p->worker].deadline > now) {
                other_replica = 1;
                break;
            }
        }
        for (uint32_t i = 1; i <= d->config.max_pages; ++i) {
            const page_entry *p = &d->pages[i];
            if (p->revision && d->workers[p->worker].deadline > now &&
                (!other_replica || p->worker == d->pages[*link].worker) &&
                kvf_hash_equal(p->manifest.key.space, m->key.space) &&
                kvf_hash_equal(p->manifest.parent, m->key.page))
                goto done;
        }
    }
    remove_page(d, link);
    ++d->stats.invalidations;
    st = KVF_OK;

done:
    pthread_mutex_unlock(&d->lock);
    return st;
}

kvf_status kvf_directory_invalidate(kvf_directory *d, const kvf_manifest *m) {
    if (!d)
        return KVF_INVALID;
    return invalidate(d, m, 0);
}

kvf_status kvf_directory_retire(kvf_directory *d, const kvf_manifest *m) {
    if (!d)
        return KVF_INVALID;
    return invalidate(d, m, 1);
}

void kvf_directory_get_stats(kvf_directory *d, kvf_directory_stats *out) {
    pthread_mutex_lock(&d->lock);
    uint64_t now = kvf_now_ms();
    expire_workers(d, now);
    *out = d->stats;
    for (uint32_t i = 0; i < d->workers_used; ++i)
        if (d->workers[i].deadline > now)
            ++out->workers;
    for (uint32_t i = 0; i < d->config.max_leases; ++i)
        if (lease_live(d, &d->leases[i], now))
            ++out->leases;
    pthread_mutex_unlock(&d->lock);
}

#ifdef KVF_DIRECTORY_TEST
/* Test-only exhaustion boundary; never present in the installed library. */
void kvf_directory_test_counters(kvf_directory *d, uint64_t fence, uint32_t slot) {
    pthread_mutex_lock(&d->lock);
    d->next_fence = fence;
    d->next_lease = slot;
    pthread_mutex_unlock(&d->lock);
}
#endif
