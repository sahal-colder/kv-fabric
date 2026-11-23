/*
 * Storage 逻辑层。逻辑记录拥有物理槽号，后端对象由调用方持有。
 * 锁顺序：Trie -> Storage -> 物理槽锁。数据 I/O 用 Lease 钉住槽后放锁。
 */
#define _POSIX_C_SOURCE 200809L
#include "tiered_internal.h"
#include "miniinfer/device_transfer.h"
#include "storage_internal.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

enum { TIER_COUNT = MI_LOCATION_SSD + 1, MAX_MIGRATIONS = 64 };

/* 逻辑块记录保存驻留槽、SSD 副本及待清理槽。 */
typedef struct block_record {
    mi_block_meta meta;        /* id matches this logical slot's generation. */
    mi_storage_handle slot;    /* Active physical slot, in meta.location. */
    mi_storage_handle replica; /* Optional validated SSD copy of this version. */
    mi_storage_handle cleanup; /* Slot that still needs release after a transition. */
    mi_location cleanup_location; /* Backend responsible for cleanup. */
    unsigned initialized : 1; /* Only a committed WRITE makes the payload readable. */
    unsigned writer : 1;      /* At most one writer and no concurrent reader. */
    unsigned busy : 1;        /* Migration owns the record and both slots outside mutex. */
} block_record;

/* 提交方和迁移线程分别持有票据引用。 */
struct mi_migration_ticket {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    atomic_uint refs;
    int done;
    mi_status status;
};

typedef struct migration_job {
    mi_block_id id;
    mi_location target;
    mi_migration_ticket *ticket;
} migration_job;

struct mi_tiered_storage {
    mi_storage_backend *backends[TIER_COUNT];
    mi_storage_backend *staging;
    mi_mempool *owned_pinned_pool;
    size_t block_bytes, capacity, free_count;
    block_record *records;
    uint32_t *free_slots, *generations;
    int retain_ssd_copy;

    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t worker;
    int worker_started, stopping;
    size_t queue_depth, queue_head, queued;
    migration_job queue[];
};

/* 由 ID 低位定位记录，并比对完整 ID 排除旧代数。 */
static block_record *record_at(mi_tiered_storage *s, mi_block_id id) {
    size_t index = (uint32_t)id - 1u;
    if (!id || index >= s->capacity || s->records[index].meta.id != id)
        return NULL;
    return &s->records[index];
}

static void recycle_record(mi_tiered_storage *s, block_record *r) {
    size_t index = (size_t)(r - s->records);
    r->meta.id = MI_INVALID_BLOCK_ID;
    if (s->generations[index] != UINT32_MAX)
        s->free_slots[s->free_count++] = (uint32_t)index;
}

void mi_storage_config_default(mi_storage_config *config) {
    if (config)
        *config = (mi_storage_config){
            .block_bytes = 1024 * 1024,
            .cpu_bytes = SIZE_MAX >= UINT64_C(34359738368) ? UINT64_C(34359738368) : SIZE_MAX,
            .max_async_migrations = 2,
        };
}

mi_status mi_tiered_storage_create(mi_storage_config config, mi_storage_backends backends,
                                   mi_tiered_storage **out) {
    if (!out || !config.block_bytes || !config.max_async_migrations ||
        config.max_async_migrations > MAX_MIGRATIONS)
        return MI_EINVAL;
    mi_storage_backend *devices[TIER_COUNT] = {backends.cpu, backends.gpu, backends.ssd};
    size_t budgets[TIER_COUNT] = {config.cpu_bytes, config.gpu_bytes, config.ssd_bytes};
    for (unsigned i = 0; i < TIER_COUNT; ++i) {
        if ((!devices[i] && budgets[i]) ||
            (devices[i] && (mi_backend_location(devices[i]) != (mi_location)i ||
                            mi_backend_slot_bytes(devices[i]) != config.block_bytes)))
            return MI_EINVAL;
    }

    mi_tiered_storage *s = calloc(1, sizeof(*s) + config.max_async_migrations * sizeof(*s->queue));
    if (!s)
        return MI_ENOMEM;
    s->block_bytes = config.block_bytes;
    s->queue_depth = config.max_async_migrations;
    s->retain_ssd_copy = config.retain_ssd_copy;

    mi_status st = MI_OK;
    if (devices[MI_LOCATION_GPU] && devices[MI_LOCATION_SSD] && config.gpu_bytes && config.ssd_bytes &&
        mi_backend_is_direct(devices[MI_LOCATION_SSD]) &&
        mi_backend_device(devices[MI_LOCATION_SSD]) != mi_backend_device(devices[MI_LOCATION_GPU])) {
        st = MI_EINVAL;
        goto failed;
    }
    if (devices[MI_LOCATION_GPU] && devices[MI_LOCATION_SSD] && config.gpu_bytes && config.ssd_bytes &&
        !mi_backend_is_direct(devices[MI_LOCATION_SSD])) {
        mi_mempool *pool = backends.pinned_pool;
        if (!pool) {
            st = mi_mempool_create(MI_MEMORY_DEVICE_PINNED, mi_backend_device(devices[MI_LOCATION_GPU]),
                                   (mi_mempool_config){0}, &s->owned_pinned_pool);
            if (st != MI_OK)
                goto failed;
            pool = s->owned_pinned_pool;
        } else if (mi_mempool_kind(pool) != MI_MEMORY_PINNED &&
                   mi_mempool_kind(pool) != MI_MEMORY_DEVICE_PINNED) {
            st = MI_EINVAL;
            goto failed;
        }
        st = mi_memory_backend_create(MI_LOCATION_CPU, config.block_bytes, pool,
                                      config.max_async_migrations, &s->staging);
        if (st != MI_OK)
            goto failed;
    }
    for (unsigned i = 0; i < TIER_COUNT; ++i) {
        if (!devices[i])
            continue;
        size_t stride = mi_backend_allocation_bytes(devices[i], config.block_bytes);
        size_t slots;
        st = mi_backend_attach(devices[i], budgets[i] / stride, &slots);
        if (st != MI_OK)
            goto failed;
        s->backends[i] = devices[i];
        if (slots > UINT32_MAX - s->capacity) {
            st = MI_EOVERFLOW;
            goto failed;
        }
        s->capacity += slots;
    }
    if (s->capacity) {
        s->records = calloc(s->capacity, sizeof(*s->records));
        s->free_slots = calloc(s->capacity, sizeof(*s->free_slots));
        s->generations = calloc(s->capacity, sizeof(*s->generations));
        if (!s->records || !s->free_slots || !s->generations) {
            st = MI_ENOMEM;
            goto failed;
        }
    }
    for (size_t i = 0; i < s->capacity; ++i)
        s->free_slots[i] = (uint32_t)(s->capacity - i - 1);
    s->free_count = s->capacity;
    st = MI_EINTERNAL;
    if (pthread_mutex_init(&s->mutex, NULL))
        goto failed;
    if (pthread_cond_init(&s->changed, NULL)) {
        pthread_mutex_destroy(&s->mutex);
        goto failed;
    }
    *out = s;
    return MI_OK;

failed:
    for (unsigned i = 0; i < TIER_COUNT; ++i)
        if (s->backends[i])
            mi_backend_detach(s->backends[i]);
    mi_backend_destroy(s->staging);
    mi_mempool_destroy(s->owned_pinned_pool);
    free(s->records);
    free(s->free_slots);
    free(s->generations);
    free(s);
    return st;
}

size_t mi_storage_block_capacity(const mi_tiered_storage *s) {
    return s->capacity;
}

mi_status mi_storage_create_block(mi_tiered_storage *s, size_t bytes, mi_location loc, mi_block_id *out) {
    if (!s || !out || !bytes || bytes > s->block_bytes || (unsigned)loc > MI_LOCATION_GPU)
        return MI_EINVAL;
    mi_storage_backend *backend = s->backends[loc];
    if (!backend)
        return MI_EUNSUPPORTED;

    mi_storage_handle slot;
    mi_status st = mi_backend_reserve(backend, bytes, &slot);
    if (st != MI_OK)
        return st;
    pthread_mutex_lock(&s->mutex);
    if (!s->free_count) {
        pthread_mutex_unlock(&s->mutex);
        (void)mi_backend_release(backend, slot);
        return MI_ENOMEM;
    }
    uint32_t index = s->free_slots[--s->free_count];
    block_record *r = &s->records[index];
    mi_block_id id = ((uint64_t)s->generations[index]++ << 32) | (index + UINT64_C(1));
    *r = (block_record){
        .meta = {.id = id, .location = loc, .size = bytes, .state = MI_BLOCK_RESIDENT},
        .slot = slot,
    };
    *out = id;
    pthread_mutex_unlock(&s->mutex);
    return MI_OK;
}

mi_status mi_storage_acquire(mi_tiered_storage *s, mi_block_id id, mi_access_mode mode,
                             mi_stream stream, mi_lease *out) {
    if (!s || !out || (unsigned)mode > MI_ACCESS_WRITE)
        return MI_EINVAL;
    *out = (mi_lease){0};
    pthread_mutex_lock(&s->mutex);
    block_record *r = record_at(s, id);
    mi_status st = MI_OK;
    if (!r)
        st = MI_ENOENT;
    else if (r->meta.location == MI_LOCATION_SSD)
        st = MI_ESTATE;
    else if (r->busy || r->cleanup || r->meta.state != MI_BLOCK_RESIDENT || r->writer ||
             r->meta.lease_count == UINT32_MAX ||
             (mode == MI_ACCESS_WRITE ? r->meta.lease_count != 0 : !r->initialized))
        st = MI_EBUSY;
    else if (!!stream != (r->meta.location == MI_LOCATION_GPU))
        st = MI_EINVAL;
    else {
        if (mode == MI_ACCESS_WRITE) {
            r->writer = 1;
            /* 写租约原地修改数据，提交前禁止读取。 */
            r->initialized = 0;
        }
        ++r->meta.lease_count;
        *out = (mi_lease){
            .storage = s, .id = id, .mode = mode,
            .data = mi_backend_reserved_ptr(s->backends[r->meta.location], r->slot),
            .size = r->meta.size, .stream = stream, .active = 1,
            .device = stream ? mi_backend_device(s->backends[MI_LOCATION_GPU]) : -1,
        };
    }
    pthread_mutex_unlock(&s->mutex);
    return st;
}

/* Lease retirement is ordered after the caller's last GPU use. */
static mi_status lease_complete(mi_lease *lease, int commit) {
    if (lease->stream) {
        mi_status st = mi_device_stream_wait(lease->device, lease->stream);
        if (st != MI_OK)
            return st;
    }
    mi_tiered_storage *s = lease->storage;
    pthread_mutex_lock(&s->mutex);
    block_record *r = record_at(s, lease->id);
    if (!r) {
        pthread_mutex_unlock(&s->mutex);
        return MI_ENOENT;
    }
    mi_status st = MI_OK;
    if (lease->mode == MI_ACCESS_WRITE) {
        r->writer = 0;
        if (commit && r->meta.version == UINT64_MAX) {
            st = MI_EOVERFLOW;
        } else if (commit) {
            r->initialized = 1;
            /* 数据提交后推进版本，读回 SSD 时用版本核对记录。 */
            ++r->meta.version;
        }
        if (r->replica) {
            if (mi_backend_release(s->backends[MI_LOCATION_SSD], r->replica) != MI_OK) {
                r->cleanup = r->replica;
                r->cleanup_location = MI_LOCATION_SSD;
                st = MI_ECLEANUP;
            }
            r->replica = MI_INVALID_HANDLE;
        }
    }
    --r->meta.lease_count;
    *lease = (mi_lease){0};
    pthread_mutex_unlock(&s->mutex);
    return st;
}

mi_status mi_lease_finish(mi_lease *lease) {
    return lease && lease->active ? lease_complete(lease, 1) : MI_EINVAL;
}

mi_status mi_lease_abort(mi_lease *lease) {
    return lease && lease->active ? lease_complete(lease, 0) : MI_EINVAL;
}

mi_status mi_storage_metadata(mi_tiered_storage *s, mi_block_id id, mi_block_meta *out) {
    if (!s || !out)
        return MI_EINVAL;
    pthread_mutex_lock(&s->mutex);
    block_record *r = record_at(s, id);
    if (r)
        *out = r->meta;
    pthread_mutex_unlock(&s->mutex);
    return r ? MI_OK : MI_ENOENT;
}

mi_status mi_storage_blocks_ready(mi_tiered_storage *s, const mi_block_id *ids, size_t n) {
    pthread_mutex_lock(&s->mutex);
    mi_status st = MI_OK;
    for (size_t i = 0; i < n; ++i) {
        const block_record *r = record_at(s, ids[i]);
        if (!r || !r->initialized || r->meta.lease_count || r->busy ||
            r->meta.state != MI_BLOCK_RESIDENT) {
            st = r ? MI_EBUSY : MI_ENOENT;
            break;
        }
    }
    pthread_mutex_unlock(&s->mutex);
    return st;
}

/* CPU 槽可直接传输，GPU 与 SSD 之间借用锁页中转槽。 */
static mi_status move_payload(mi_storage_backend *staging, mi_storage_backend *src, mi_storage_handle old_slot,
                               mi_storage_backend *dst, mi_storage_handle new_slot,
                               mi_block_meta meta, mi_location target) {
    if (meta.location == MI_LOCATION_CPU) {
        void *host = mi_backend_reserved_ptr(src, old_slot);
        if (target == MI_LOCATION_SSD) {
            meta.location = target;
            return mi_backend_write_record(dst, new_slot, &meta, host);
        }
        return mi_backend_write(dst, new_slot, 0, host, meta.size);
    }

    if (target == MI_LOCATION_CPU) {
        void *host = mi_backend_reserved_ptr(dst, new_slot);
        return meta.location == MI_LOCATION_SSD
            ? mi_backend_read_validated(src, old_slot, &meta, host)
            : mi_backend_read(src, old_slot, 0, host, meta.size);
    }

    if (target == MI_LOCATION_SSD && mi_backend_is_direct(dst)) {
        meta.location = target;
        return mi_backend_direct_move(dst, new_slot, &meta, mi_backend_reserved_ptr(src, old_slot),
                                       mi_backend_device(src), 1);
    }
    if (meta.location == MI_LOCATION_SSD && mi_backend_is_direct(src))
        return mi_backend_direct_move(src, old_slot, &meta, mi_backend_reserved_ptr(dst, new_slot),
                                       mi_backend_device(dst), 0);

    mi_storage_handle staging_slot;
    mi_status st = mi_backend_reserve(staging, meta.size, &staging_slot);
    if (st != MI_OK)
        return st;
    void *host = mi_backend_reserved_ptr(staging, staging_slot);
    if (target == MI_LOCATION_SSD) {
        st = mi_backend_read(src, old_slot, 0, host, meta.size);
        if (st == MI_OK) {
            meta.location = target;
            st = mi_backend_write_record(dst, new_slot, &meta, host);
        }
    } else {
        st = mi_backend_read_validated(src, old_slot, &meta, host);
        if (st == MI_OK)
            st = mi_backend_write(dst, new_slot, 0, host, meta.size);
    }
    (void)mi_backend_release(staging, staging_slot);
    return st;
}

mi_status mi_storage_migrate(mi_tiered_storage *s, mi_block_id id, mi_location target) {
    if (!s || (unsigned)target >= TIER_COUNT)
        return MI_EINVAL;
    pthread_mutex_lock(&s->mutex);
    block_record *r = record_at(s, id);
    mi_storage_backend *dst = s->backends[target];
    mi_status st = MI_OK;
    if (!r)
        st = MI_ENOENT;
    else if (r->busy || r->meta.lease_count || r->cleanup || r->meta.state != MI_BLOCK_RESIDENT)
        st = MI_EBUSY;
    else if (!dst)
        st = MI_EUNSUPPORTED;
    else if (target == MI_LOCATION_SSD && !r->initialized)
        st = MI_ESTATE;
    if (st != MI_OK || r->meta.location == target) {
        pthread_mutex_unlock(&s->mutex);
        return st;
    }
    mi_block_meta meta = r->meta;
    mi_storage_backend *src = s->backends[meta.location];
    mi_storage_handle old_slot = r->slot, new_slot = MI_INVALID_HANDLE;
    int initialized = r->initialized;
    /* 锁外搬运期间阻止并发访问同一个块。 */
    r->busy = 1;
    r->meta.state = MI_BLOCK_MOVING;
    pthread_mutex_unlock(&s->mutex);

    st = mi_backend_reserve(dst, meta.size, &new_slot);
    if (st == MI_OK && r->replica) {
        st = mi_backend_release(s->backends[MI_LOCATION_SSD], r->replica);
        if (st == MI_OK)
            r->replica = MI_INVALID_HANDLE;
    }
    if (st == MI_OK && initialized)
        st = move_payload(s->staging, src, old_slot, dst, new_slot, meta, target);
    pthread_mutex_lock(&s->mutex);
    if (st == MI_OK) {
        /* 搬运成功后切换驻留槽，逻辑 ID 保持不变。 */
        r->slot = new_slot;
        r->meta.location = target;
        if (meta.location == MI_LOCATION_SSD && s->retain_ssd_copy) {
            r->replica = old_slot;
        } else if (mi_backend_release(src, old_slot) != MI_OK) {
            r->cleanup = old_slot;
            r->cleanup_location = meta.location;
            st = MI_ECLEANUP;
        }
    } else if (new_slot && mi_backend_release(dst, new_slot) != MI_OK) {
        /* 回滚时无法释放目标槽，留待后续清理。 */
        r->cleanup = new_slot;
        r->cleanup_location = target;
        st = MI_EROLLBACK_CLEANUP;
    }
    r->meta.state = MI_BLOCK_RESIDENT;
    r->busy = 0;
    pthread_mutex_unlock(&s->mutex);
    return st;
}

mi_status mi_storage_migrate_batch(mi_tiered_storage *s, const mi_block_id *ids, size_t count,
                                   mi_location target, mi_status *results) {
    if (!s || !ids || !results || !count || count > MAX_MIGRATIONS)
        return MI_EINVAL;
    mi_status first = MI_OK;
    for (size_t i = 0; i < count; ++i) {
        results[i] = mi_storage_migrate(s, ids[i], target);
        if (first == MI_OK)
            first = results[i];
    }
    return first;
}

static void ticket_release(mi_migration_ticket *t) {
    if (atomic_fetch_sub_explicit(&t->refs, 1, memory_order_acq_rel) == 1) {
        pthread_cond_destroy(&t->changed);
        pthread_mutex_destroy(&t->mutex);
        free(t);
    }
}

/* 顺序执行队列中的迁移，并将结果发布到票据。 */
static void *migration_worker(void *arg) {
    mi_tiered_storage *s = arg;
    pthread_mutex_lock(&s->mutex);
    for (;;) {
        while (!s->queued && !s->stopping)
            pthread_cond_wait(&s->changed, &s->mutex);
        if (!s->queued) {
            pthread_mutex_unlock(&s->mutex);
            return NULL;
        }
        migration_job job = s->queue[s->queue_head];
        pthread_mutex_unlock(&s->mutex);

        mi_status st = mi_storage_migrate(s, job.id, job.target);

        pthread_mutex_lock(&s->mutex);
        s->queue_head = (s->queue_head + 1) % s->queue_depth;
        --s->queued;
        pthread_mutex_unlock(&s->mutex);
        pthread_mutex_lock(&job.ticket->mutex);
        job.ticket->status = st;
        job.ticket->done = 1;
        pthread_cond_broadcast(&job.ticket->changed);
        pthread_mutex_unlock(&job.ticket->mutex);
        ticket_release(job.ticket);
        pthread_mutex_lock(&s->mutex);
    }
}

/* 将任务加入有界队列，票据保留结果。 */
mi_status mi_storage_migrate_async(mi_tiered_storage *s, mi_block_id id, mi_location target,
                                   mi_migration_ticket **out) {
    if (!s || !out || (unsigned)target >= TIER_COUNT)
        return MI_EINVAL;
    mi_migration_ticket *t = calloc(1, sizeof(*t));
    if (!t)
        return MI_ENOMEM;
    if (pthread_mutex_init(&t->mutex, NULL)) {
        free(t);
        return MI_EINTERNAL;
    }
    if (pthread_cond_init(&t->changed, NULL)) {
        pthread_mutex_destroy(&t->mutex);
        free(t);
        return MI_EINTERNAL;
    }
    atomic_init(&t->refs, 1);
    pthread_mutex_lock(&s->mutex);
    mi_status st = MI_OK;
    if (s->stopping || s->queued == s->queue_depth)
        st = MI_EBUSY;
    else if (!s->worker_started && pthread_create(&s->worker, NULL, migration_worker, s))
        st = MI_EINTERNAL;
    else {
        s->worker_started = 1;
        atomic_fetch_add_explicit(&t->refs, 1, memory_order_relaxed);
        s->queue[(s->queue_head + s->queued++) % s->queue_depth] = (migration_job){id, target, t};
        pthread_cond_signal(&s->changed);
    }
    pthread_mutex_unlock(&s->mutex);
    if (st == MI_OK)
        *out = t;
    else
        ticket_release(t);
    return st;
}

mi_status mi_migration_wait(mi_migration_ticket *t) {
    if (!t)
        return MI_EINVAL;
    pthread_mutex_lock(&t->mutex);
    while (!t->done)
        pthread_cond_wait(&t->changed, &t->mutex);
    mi_status st = t->status;
    pthread_mutex_unlock(&t->mutex);
    return st;
}

void mi_migration_ticket_destroy(mi_migration_ticket *t) {
    if (t)
        ticket_release(t);
}

mi_status mi_storage_read_to_host(mi_tiered_storage *s, mi_block_id id, void *dst,
                                  size_t capacity, size_t *bytes_read) {
    if (!s || !dst)
        return MI_EINVAL;
    pthread_mutex_lock(&s->mutex);
    block_record *r = record_at(s, id);
    if (!r || !r->initialized || r->meta.lease_count || r->busy ||
        r->meta.state != MI_BLOCK_RESIDENT || capacity < r->meta.size) {
        mi_status st = !r ? MI_ENOENT : capacity < r->meta.size ? MI_EINVAL : MI_EBUSY;
        pthread_mutex_unlock(&s->mutex);
        return st;
    }
    mi_block_meta meta = r->meta;
    mi_storage_handle slot = r->slot;
    mi_storage_backend *b = s->backends[meta.location];
    /* 临时持有读引用，防止锁外读取时块被迁移或释放。 */
    ++r->meta.lease_count;
    pthread_mutex_unlock(&s->mutex);
    mi_status st = mi_backend_read_validated(b, slot, &meta, dst);
    pthread_mutex_lock(&s->mutex);
    --r->meta.lease_count;
    pthread_mutex_unlock(&s->mutex);
    if (st == MI_OK && bytes_read)
        *bytes_read = meta.size;
    return st;
}

int mi_storage_gpu_device(const mi_tiered_storage *s) {
    return s && s->backends[MI_LOCATION_GPU] ? mi_backend_device(s->backends[MI_LOCATION_GPU]) : -1;
}

mi_status mi_storage_read_snapshot_event(mi_tiered_storage *s, mi_block_id id, uint64_t version,
                                         void *dst, size_t capacity, mi_stream stream, mi_event event,
                                         mi_block_meta *out) {
    if (!s || !dst || !out)
        return MI_EINVAL;
    pthread_mutex_lock(&s->mutex);
    block_record *r = record_at(s, id);
    mi_status st = MI_OK;
    if (!r || (version && (r->meta.version != version || r->meta.size != capacity)))
        st = MI_ENOENT;
    else if (capacity < r->meta.size || (r->meta.location == MI_LOCATION_GPU && !stream))
        st = MI_EINVAL;
    else if (!r->initialized || r->writer || r->busy || r->cleanup ||
             r->meta.state != MI_BLOCK_RESIDENT || r->meta.lease_count == UINT32_MAX)
        st = MI_EBUSY;
    if (st != MI_OK) {
        pthread_mutex_unlock(&s->mutex);
        return st;
    }
    mi_block_meta meta = r->meta;
    mi_storage_backend *b = s->backends[meta.location];
    mi_storage_handle slot = r->slot;
    /* Check the generation/version and take the READ pin before dropping metadata.
     * Migration and slot reuse must see the pin even if the caller is descheduled. */
    ++r->meta.lease_count;
    pthread_mutex_unlock(&s->mutex);

    if (meta.location == MI_LOCATION_GPU) {
        int device = mi_backend_device(b);
        st = mi_device_copy_to_host(device, dst, mi_backend_reserved_ptr(b, slot), meta.size, stream);
        if (st == MI_OK && event) {
            st = mi_device_event_record(device, event, stream);
            if (st == MI_OK)
                st = mi_device_event_wait(device, event);
            if (st != MI_OK)
                (void)mi_device_stream_wait(device, stream);
        } else if (st == MI_OK) {
            st = mi_device_stream_wait(device, stream);
        }
    } else {
        st = mi_backend_read_validated(b, slot, &meta, dst);
    }
    pthread_mutex_lock(&s->mutex);
    --r->meta.lease_count;
    pthread_mutex_unlock(&s->mutex);
    if (st == MI_OK)
        *out = meta;
    return st;
}

mi_status mi_storage_read_snapshot(mi_tiered_storage *s, mi_block_id id, uint64_t version,
                                   void *dst, size_t capacity, mi_stream stream, mi_block_meta *out) {
    return mi_storage_read_snapshot_event(s, id, version, dst, capacity, stream, NULL, out);
}

static mi_status clear_cleanup(mi_tiered_storage *s, block_record *r) {
    if (!r->cleanup)
        return MI_OK;
    mi_status st = mi_backend_release(s->backends[r->cleanup_location], r->cleanup);
    if (st == MI_OK)
        r->cleanup = MI_INVALID_HANDLE;
    return st;
}

mi_status mi_storage_retry_cleanup(mi_tiered_storage *s, mi_block_id id) {
    if (!s)
        return MI_EINVAL;
    pthread_mutex_lock(&s->mutex);
    block_record *r = record_at(s, id);
    mi_status st = !r ? MI_ENOENT : r->busy || r->meta.lease_count ? MI_EBUSY : clear_cleanup(s, r);
    if (st == MI_OK && !r->slot)
        recycle_record(s, r);
    pthread_mutex_unlock(&s->mutex);
    return st;
}

mi_status mi_storage_erase(mi_tiered_storage *s, mi_block_id id) {
    if (!s)
        return MI_EINVAL;
    pthread_mutex_lock(&s->mutex);
    block_record *r = record_at(s, id);
    if (!r || r->busy || r->meta.lease_count) {
        pthread_mutex_unlock(&s->mutex);
        return r ? MI_EBUSY : MI_ENOENT;
    }
    mi_status st = clear_cleanup(s, r);
    if (st == MI_OK && r->slot) {
        st = mi_backend_release(s->backends[r->meta.location], r->slot);
        if (st == MI_OK)
            r->slot = MI_INVALID_HANDLE;
        else
            r->meta.state = MI_BLOCK_ERROR;
    }
    if (st == MI_OK && r->replica &&
        mi_backend_release(s->backends[MI_LOCATION_SSD], r->replica) != MI_OK) {
        r->cleanup = r->replica;
        r->cleanup_location = MI_LOCATION_SSD;
        r->replica = MI_INVALID_HANDLE;
        r->meta.state = MI_BLOCK_ERROR;
        st = MI_ECLEANUP;
    }
    if (st == MI_OK)
        recycle_record(s, r);
    pthread_mutex_unlock(&s->mutex);
    return st;
}

int mi_storage_has_room(mi_tiered_storage *s, mi_location loc) {
    return s && (unsigned)loc < TIER_COUNT && s->backends[loc] &&
        mi_backend_available(s->backends[loc]) != 0;
}

void mi_tiered_storage_destroy(mi_tiered_storage *s) {
    if (!s)
        return;
    pthread_mutex_lock(&s->mutex);
    s->stopping = 1;
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->mutex);
    if (s->worker_started)
        pthread_join(s->worker, NULL);
    for (size_t i = 0; i < s->capacity; ++i) {
        block_record *r = &s->records[i];
        if (!r->meta.id)
            continue;
        (void)clear_cleanup(s, r);
        if (r->replica)
            (void)mi_backend_release(s->backends[MI_LOCATION_SSD], r->replica);
        if (r->slot)
            (void)mi_backend_release(s->backends[r->meta.location], r->slot);
    }
    for (unsigned i = 0; i < TIER_COUNT; ++i)
        if (s->backends[i])
            mi_backend_detach(s->backends[i]);
    mi_backend_destroy(s->staging);
    mi_mempool_destroy(s->owned_pinned_pool);
    free(s->records);
    free(s->free_slots);
    free(s->generations);
    pthread_cond_destroy(&s->changed);
    pthread_mutex_destroy(&s->mutex);
    free(s);
}
