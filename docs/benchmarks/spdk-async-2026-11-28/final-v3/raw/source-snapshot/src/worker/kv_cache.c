/*
 * 本地 KV Offload 层。管理冷热位置、迁移和预取。
 * 缓存持有条目和 Block ID，借用 Storage。先保护缓存元数据再获取块引用。
 */
#include "miniinfer/kv_cache.h"
#include <stdlib.h>
#include <string.h>

enum { PREFETCH_NONE, PREFETCH_PENDING, PREFETCH_DONE };

typedef struct mi_kv_entry {
    mi_block_id id;
    size_t prev, next;
    int prefetch;
    mi_status status;
} mi_kv_entry;

/* 任务携带提交时的块 ID，用来识别已被替换的槽。 */
typedef struct mi_kv_job {
    size_t slot;
    mi_block_id id;
} mi_kv_job;

/* Only CPU candidates are linked, with one-based indices and zero as the sentinel. */
static void lru_remove(mi_kv_cache *c, size_t index) {
    mi_kv_entry *e = &c->entries[index - 1];
    if (!e->prev && c->oldest != index)
        return;
    if (e->prev)
        c->entries[e->prev - 1].next = e->next;
    else
        c->oldest = e->next;
    if (e->next)
        c->entries[e->next - 1].prev = e->prev;
    else
        c->newest = e->prev;
    e->prev = e->next = 0;
}

/* 将刚访问的 CPU 驻留槽移到 LRU 尾部。 */
static void lru_touch(mi_kv_cache *c, size_t index) {
    lru_remove(c, index);
    if (c->newest)
        c->entries[c->newest - 1].next = index;
    else
        c->oldest = index;
    c->entries[index - 1].prev = c->newest;
    c->newest = index;
}

/* CPU 无空闲槽时，优先迁出最久未访问的槽。 */
static mi_status make_room(mi_kv_cache *c) {
    if (mi_storage_has_room(c->storage, MI_LOCATION_CPU))
        return MI_OK;
    size_t oldest = c->oldest;
    if (!oldest)
        return MI_ENOMEM;
    mi_block_id id = c->entries[oldest - 1].id;
    mi_status st = mi_storage_migrate(c->storage, id, MI_LOCATION_SSD);
    if (st == MI_OK || st == MI_ECLEANUP) {
        lru_remove(c, oldest);
        if (st == MI_ECLEANUP)
            st = mi_storage_retry_cleanup(c->storage, id);
    }
    return st == MI_EUNSUPPORTED ? MI_ENOMEM : st;
}

/* SSD 命中时先腾出 CPU 槽，再迁回数据。 */
static mi_status promote(mi_kv_cache *c, size_t slot) {
    mi_kv_entry *e = &c->entries[slot];
    if (!e->id)
        return MI_ENOENT;
    mi_block_meta meta;
    mi_status st = mi_storage_metadata(c->storage, e->id, &meta);
    if (st != MI_OK)
        return st;
    if (meta.location == MI_LOCATION_SSD) {
        st = make_room(c);
        if (st == MI_OK)
            st = mi_storage_migrate(c->storage, e->id, MI_LOCATION_CPU);
        if (st == MI_ECLEANUP)
            st = mi_storage_retry_cleanup(c->storage, e->id);
    } else if (meta.location != MI_LOCATION_CPU) {
        st = MI_ESTATE;
    }
    if (st == MI_OK)
        lru_touch(c, slot + 1);
    return st;
}

static void *prefetch_worker(void *arg) {
    mi_kv_cache *c = arg;
    for (;;) {
        pthread_mutex_lock(&c->mutex);
        while (!c->queued && !c->stopping)
            pthread_cond_wait(&c->work, &c->mutex);
        if (!c->queued) {
            pthread_mutex_unlock(&c->mutex);
            return NULL;
        }
        mi_kv_job job = c->queue[c->queue_head];
        c->queue_head = (c->queue_head + 1) % c->config.prefetch_queue_depth;
        --c->queued;
        mi_kv_entry *e = &c->entries[job.slot];
        /* 槽被覆写后，跳过引用旧块的预取任务。 */
        if (e->id == job.id) {
            e->status = promote(c, job.slot);
            e->prefetch = PREFETCH_DONE;
            pthread_cond_broadcast(&c->completed);
        }
        pthread_mutex_unlock(&c->mutex);
    }
}

mi_status mi_kv_cache_create(mi_tiered_storage *storage, mi_kv_cache_config cfg, mi_kv_cache **out) {
    if (!storage || !out || !cfg.logical_slots || !cfg.payload_bytes)
        return MI_EINVAL;
    mi_kv_cache *c = calloc(1, sizeof(*c));
    if (!c)
        return MI_ENOMEM;
    c->storage = storage;
    c->config = cfg;
    c->entries = calloc(cfg.logical_slots, sizeof(*c->entries));
    int worker = cfg.enable_prefetch_worker && cfg.prefetch_queue_depth;
    if (worker)
        c->queue = calloc(cfg.prefetch_queue_depth, sizeof(*c->queue));
    mi_status st = MI_ENOMEM;
    if (!c->entries || (worker && !c->queue))
        goto failed;
    st = MI_EINTERNAL;
    if (pthread_mutex_init(&c->mutex, NULL))
        goto failed;
    if (pthread_cond_init(&c->work, NULL))
        goto destroy_mutex;
    if (pthread_cond_init(&c->completed, NULL))
        goto destroy_work;
    if (worker && pthread_create(&c->worker, NULL, prefetch_worker, c))
        goto destroy_completed;
    c->worker_started = worker;
    *out = c;
    return MI_OK;

destroy_completed:
    pthread_cond_destroy(&c->completed);
destroy_work:
    pthread_cond_destroy(&c->work);
destroy_mutex:
    pthread_mutex_destroy(&c->mutex);
failed:
    free(c->queue);
    free(c->entries);
    free(c);
    return st;
}

static mi_status erase(mi_kv_cache *c, size_t slot) {
    mi_kv_entry *e = &c->entries[slot];
    if (!e->id)
        return MI_OK;
    mi_status st = mi_storage_erase(c->storage, e->id);
    if (st == MI_OK || st == MI_ENOENT) {
        lru_remove(c, slot + 1);
        *e = (mi_kv_entry){0};
        pthread_cond_broadcast(&c->completed);
        return MI_OK;
    }
    return st;
}

mi_status mi_kv_cache_write(mi_kv_cache *c, size_t slot, const void *data, size_t bytes) {
    if (!c || slot >= c->config.logical_slots || !data || !bytes || bytes > c->config.payload_bytes)
        return MI_EINVAL;
    pthread_mutex_lock(&c->mutex);
    mi_status st = erase(c, slot);
    mi_block_id id;
    if (st == MI_OK)
        st = make_room(c);
    if (st == MI_OK)
        st = mi_storage_create_block(c->storage, bytes, MI_LOCATION_CPU, &id);
    if (st == MI_OK) {
        mi_lease lease;
        st = mi_storage_acquire(c->storage, id, MI_ACCESS_WRITE, NULL, &lease);
        if (st == MI_OK) {
            memcpy(lease.data, data, bytes);
            st = mi_lease_finish(&lease);
        }
        if (st == MI_OK) {
            c->entries[slot].id = id;
            lru_touch(c, slot + 1);
        } else {
            (void)mi_storage_erase(c->storage, id);
        }
    }
    pthread_mutex_unlock(&c->mutex);
    return st;
}

mi_status mi_kv_cache_read(mi_kv_cache *c, size_t slot, void *data, size_t capacity,
                           size_t *bytes_read) {
    if (!c || slot >= c->config.logical_slots || !data)
        return MI_EINVAL;
    pthread_mutex_lock(&c->mutex);
    mi_block_id id = c->entries[slot].id;
    mi_block_meta meta;
    mi_status st = id ? mi_storage_metadata(c->storage, id, &meta) : MI_ENOENT;
    if (st == MI_OK)
        st = capacity < meta.size ? MI_EINVAL : promote(c, slot);
    if (st == MI_OK)
        st = mi_storage_read_to_host(c->storage, id, data, capacity, bytes_read);
    pthread_mutex_unlock(&c->mutex);
    return st;
}

mi_status mi_kv_cache_prefetch(mi_kv_cache *c, size_t slot) {
    if (!c || slot >= c->config.logical_slots)
        return MI_EINVAL;
    pthread_mutex_lock(&c->mutex);
    mi_status st = promote(c, slot);
    pthread_mutex_unlock(&c->mutex);
    return st;
}

mi_status mi_kv_cache_prefetch_async(mi_kv_cache *c, size_t slot) {
    if (!c || slot >= c->config.logical_slots || !c->worker_started)
        return MI_EUNSUPPORTED;
    pthread_mutex_lock(&c->mutex);
    mi_kv_entry *e = &c->entries[slot];
    mi_status st = MI_OK;
    if (!e->id)
        st = MI_ENOENT;
    else if (e->prefetch != PREFETCH_PENDING) {
        size_t depth = c->config.prefetch_queue_depth;
        if (c->stopping || c->queued == depth) {
            st = MI_EBUSY;
        } else {
            c->queue[(c->queue_head + c->queued) % depth] = (mi_kv_job){slot, e->id};
            ++c->queued;
            e->prefetch = PREFETCH_PENDING;
            pthread_cond_signal(&c->work);
        }
    }
    pthread_mutex_unlock(&c->mutex);
    return st;
}

mi_status mi_kv_cache_wait_prefetch(mi_kv_cache *c, size_t slot) {
    if (!c || slot >= c->config.logical_slots)
        return MI_EINVAL;
    pthread_mutex_lock(&c->mutex);
    mi_kv_entry *e = &c->entries[slot];
    mi_block_id id = e->id;
    while (e->id == id && e->prefetch == PREFETCH_PENDING)
        pthread_cond_wait(&c->completed, &c->mutex);
    mi_status st = e->id == id && e->prefetch == PREFETCH_DONE ? e->status : MI_ESTATE;
    pthread_mutex_unlock(&c->mutex);
    return st;
}

mi_status mi_kv_cache_erase(mi_kv_cache *c, size_t slot) {
    if (!c || slot >= c->config.logical_slots)
        return MI_EINVAL;
    pthread_mutex_lock(&c->mutex);
    mi_status st = erase(c, slot);
    pthread_mutex_unlock(&c->mutex);
    return st;
}

void mi_kv_cache_destroy(mi_kv_cache *c) {
    if (!c)
        return;
    pthread_mutex_lock(&c->mutex);
    c->stopping = 1;
    pthread_cond_signal(&c->work);
    pthread_mutex_unlock(&c->mutex);
    if (c->worker_started)
        pthread_join(c->worker, NULL);
    for (size_t i = 0; i < c->config.logical_slots; ++i)
        (void)erase(c, i);
    pthread_cond_destroy(&c->completed);
    pthread_cond_destroy(&c->work);
    pthread_mutex_destroy(&c->mutex);
    free(c->queue);
    free(c->entries);
    free(c);
}
