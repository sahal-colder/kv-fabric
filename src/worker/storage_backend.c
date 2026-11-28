/*
 * Storage 物理层。对象拥有 slot 元数据及 SSD 资源，内存 Slab 借自调用方的池。
 * 锁顺序：Storage -> Backend 槽锁；放开槽锁才执行设备或定位 I/O。
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include "storage_internal.h"
#include "allocator_internal.h"
#include "miniinfer/device_transfer.h"
#include "direct_io.h"
#include "storage_record.h"
#include "io_transfer.h"
#include "segment_pool.h"
#ifdef MINIINFER_WITH_SPDK
#include "spdk_bridge.h"
#endif
#include <fcntl.h>
#include <errno.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { HEADER_BYTES = 64, SLOT_ALIGNMENT = 256, SPDK_SECTOR = 4096,
       GPU_TRANSFER_SLOTS = 128 }; /* Match the maximum concurrent Worker lanes. */
typedef enum { BACKEND_MEMORY, BACKEND_POSIX, BACKEND_URING, BACKEND_SPDK, BACKEND_DIRECT } backend_kind;
static const char ssd_magic[8] = "MIFSSD1";

typedef struct storage_slot {
    uint32_t generation;
    unsigned used;
} storage_slot;

/* 分别记录 SSD 槽的写入进度和封存后的可读长度。 */
typedef struct ssd_slot {
    size_t written, sealed_bytes;
} ssd_slot;

/* The handle packs a generation above a one-based physical slot index. */
struct mi_storage_backend {
    backend_kind kind;
    mi_location location;
    size_t slot_bytes, stride, capacity, limit, free_count;
    uint32_t *free_slots;
    storage_slot *slots;
    int attached, device;
    pthread_mutex_t mutex;
    pthread_cond_t io_changed;
    union {
        struct {
            mi_mempool *pool;
            unsigned char *base;
            mi_segment_pool *transfers;
        } memory;
        struct {
            ssd_slot *records;
            mi_ssd_verify verify;
            int fd;
            uint64_t base_offset;
            mi_direct_io *direct;
            mi_segment_pool *records_pool;
#ifdef MINIINFER_WITH_URING
            mi_uring *ring;
#endif
#ifdef MINIINFER_WITH_SPDK
            mi_spdk_device *spdk;
#endif
        } ssd;
    };
    /* Keep fault-only state away from the CPU slot/mutex cache lines. */
    atomic_uint_fast64_t metrics[5];
    atomic_bool gpu_pending;
    struct mi_storage_backend *retired_next;
    int retired, destroy_requested;
};

static pthread_mutex_t retired_mutex = PTHREAD_MUTEX_INITIALIZER;
static mi_storage_backend *retired_backends;

static size_t slot_index(mi_storage_handle handle) {
    return (uint32_t)handle - 1u;
}

/* 同时验证槽占用状态与句柄代数。 */
static int owns_slot(const mi_storage_backend *b, mi_storage_handle handle) {
    size_t i = slot_index(handle);
    return i < b->capacity && b->slots[i].used && b->slots[i].generation == (handle >> 32);
}

static mi_status backend_create(backend_kind kind, mi_location location, size_t slot_bytes,
                                 size_t stride, size_t count, mi_storage_backend **out) {
    if (count > UINT32_MAX || count > SIZE_MAX / sizeof(ssd_slot))
        return MI_EOVERFLOW;
    mi_storage_backend *b = calloc(1, sizeof(*b));
    if (!b)
        return MI_ENOMEM;
    b->slots = calloc(count, sizeof(*b->slots));
    b->free_slots = malloc(count * sizeof(*b->free_slots));
    if (kind != BACKEND_MEMORY)
        b->ssd.records = calloc(count, sizeof(*b->ssd.records));
    mi_status st = MI_ENOMEM;
    if (!b->slots || !b->free_slots || (kind != BACKEND_MEMORY && !b->ssd.records))
        goto failed;
    st = MI_EINTERNAL;
    if (pthread_mutex_init(&b->mutex, NULL) != 0)
        goto failed;

    for(unsigned i=0;i<5;++i) atomic_init(&b->metrics[i],0);
    if (pthread_cond_init(&b->io_changed, NULL)) { pthread_mutex_destroy(&b->mutex); goto failed; }
    b->kind = kind;
    b->location = location;
    b->device = -1;
    atomic_init(&b->gpu_pending, 0);
    b->slot_bytes = slot_bytes;
    b->stride = stride;
    b->capacity = b->limit = b->free_count = count;
    if (kind != BACKEND_MEMORY)
        b->ssd.fd = -1;
    for (size_t i = 0; i < count; ++i)
        b->free_slots[i] = (uint32_t)(count - i - 1);
    *out = b;
    return MI_OK;

failed:
    if (kind != BACKEND_MEMORY)
        free(b->ssd.records);
    free(b->free_slots);
    free(b->slots);
    free(b);
    return st;
}

mi_status mi_memory_backend_create(mi_location location, size_t bytes, mi_mempool *pool,
                                   size_t max_slots, mi_storage_backend **out) {
    if (!out || !pool || !bytes || !max_slots ||
        (location != MI_LOCATION_CPU && location != MI_LOCATION_GPU) ||
        (location == MI_LOCATION_GPU) != (mi_mempool_kind(pool) == MI_MEMORY_DEVICE))
        return MI_EINVAL;
    mi_memory_kind kind = mi_mempool_kind(pool);
    size_t alignment = kind == MI_MEMORY_PINNED || kind == MI_MEMORY_DEVICE_PINNED
        ? SPDK_SECTOR : SLOT_ALIGNMENT;
    if (bytes > SIZE_MAX - (alignment - 1))
        return MI_EOVERFLOW;
    size_t stride = (bytes + alignment - 1) & ~(alignment - 1);
    if (max_slots > SIZE_MAX / stride)
        return MI_EOVERFLOW;
    mi_storage_backend *b;
    mi_status st = backend_create(BACKEND_MEMORY, location, bytes, stride, max_slots, &b);
    if (st != MI_OK)
        return st;

    void *slab = NULL;
    st = mi_mempool_alloc_slab(pool, max_slots * stride, alignment, &slab);
    if (st != MI_OK) {
        mi_backend_destroy(b);
        return st;
    }
    b->memory.pool = pool;
    mi_mempool_hold(pool);
    b->memory.base = slab;
    b->device = mi_mempool_device(pool);
    if (location == MI_LOCATION_GPU) {
        /* Fixed, bounded ownership: no DMA ever outlives borrowed host memory.
         * Cover every serving lane without reducing existing concurrency. */
        st = mi_segment_pool_create(b->device, bytes, GPU_TRANSFER_SLOTS, 1, &b->memory.transfers);
        if (st != MI_OK) {
            mi_backend_destroy(b);
            return st;
        }
    }
    *out = b;
    return MI_OK;
}

/* 把记录头与对齐填充计入 SSD 槽步长。 */
static mi_status ssd_layout(size_t bytes, size_t capacity, size_t header, size_t alignment, size_t *stride) {
    if (bytes > SIZE_MAX - header - (alignment - 1))
        return MI_EOVERFLOW;
    *stride = (bytes + header + alignment - 1) & ~(alignment - 1);
    return capacity < *stride || capacity > INT64_MAX ? MI_ENOMEM : MI_OK;
}

static mi_status open_file(const char *path, const char *dir, size_t bytes, size_t capacity,
                            mi_ssd_verify verify, backend_kind kind, unsigned depth, int device,
                            mi_storage_backend **out) {
    if (!out || !bytes || (unsigned)verify > MI_SSD_VERIFY_READ_BACK ||
        ((kind == BACKEND_URING || kind == BACKEND_DIRECT) && (!depth || depth > 128)))
        return MI_EINVAL;
    *out = NULL;
#ifndef MINIINFER_WITH_URING
    if (kind == BACKEND_URING)
        return MI_EUNSUPPORTED;
#endif
    size_t stride;
    mi_status st = ssd_layout(bytes, capacity, kind == BACKEND_DIRECT ? MI_DIRECT_HEADER_BYTES : HEADER_BYTES,
                              kind == BACKEND_DIRECT ? SPDK_SECTOR : 1, &stride);
    if (st != MI_OK)
        return st;

    size_t slots = capacity / stride;
    mi_storage_backend *b;
    st = backend_create(kind, MI_LOCATION_SSD, bytes, stride, slots, &b);
    if (st != MI_OK)
        return st;
    int fd;
    if (path) {
        fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    } else {
        static const char suffix[] = "/miniinfer-ssd-XXXXXX";
        size_t len = strlen(dir);
        if (len > SIZE_MAX - sizeof(suffix)) {
            st = MI_EOVERFLOW;
            goto failed;
        }
        char *temp = malloc(len + sizeof(suffix));
        if (!temp) {
            st = MI_ENOMEM;
            goto failed;
        }
        memcpy(temp, dir, len);
        memcpy(temp + len, suffix, sizeof(suffix));
        /* 临时后端打开后立即移除目录项，描述符仍可读写。 */
        fd = mkstemp(temp);
        if (fd >= 0 && (unlink(temp) != 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0)) {
            close(fd);
            fd = -1;
        }
        free(temp);
    }
    st = MI_EIO;
    if (fd < 0)
        goto failed;
    b->ssd.fd = fd;
#ifdef MINIINFER_WITH_URING
    if (kind == BACKEND_URING) {
        st = mi_uring_create(fd, depth, &b->ssd.ring);
        if (st != MI_OK)
            goto failed;
    }
#endif
    if (kind == BACKEND_DIRECT) {
        st = mi_direct_create(fd, device, stride, depth, &b->ssd.direct);
        if (st != MI_OK)
            goto failed;
        b->device = device;
    }
    st = MI_EIO;
    if (ftruncate(fd, 0) != 0 || ftruncate(fd, (off_t)(slots * stride)) != 0)
        goto failed;
    b->ssd.verify = verify;
    *out = b;
    return MI_OK;

failed:
    mi_backend_destroy(b);
    return st;
}

mi_status mi_ssd_file_backend_create(const char *path, size_t bytes, size_t capacity,
                                     mi_ssd_verify verify, mi_ssd_file_mode mode,
                                     unsigned depth, mi_storage_backend **out) {
    if ((unsigned)mode > MI_SSD_FILE_URING)
        return MI_EINVAL;
    return open_file(path, "/tmp", bytes, capacity, verify,
                     mode == MI_SSD_FILE_URING ? BACKEND_URING : BACKEND_POSIX, depth, -1, out);
}

mi_status mi_ssd_temp_backend_create(const char *dir, size_t bytes, size_t capacity,
                                     mi_ssd_verify verify, mi_ssd_file_mode mode,
                                     unsigned depth, mi_storage_backend **out) {
    if (!dir || !*dir || (unsigned)mode > MI_SSD_FILE_URING)
        return MI_EINVAL;
    return open_file(NULL, dir, bytes, capacity, verify,
                     mode == MI_SSD_FILE_URING ? BACKEND_URING : BACKEND_POSIX, depth, -1, out);
}

mi_status mi_ssd_backend_create(const char *path, size_t bytes, size_t capacity,
                                mi_ssd_verify verify, mi_storage_backend **out) {
    return mi_ssd_file_backend_create(path, bytes, capacity, verify, MI_SSD_FILE_POSIX, 1, out);
}

mi_status mi_ssd_direct_backend_create(const char *path, size_t bytes, size_t capacity,
                                       mi_ssd_verify verify, unsigned depth, int device,
                                       mi_storage_backend **out) {
    return open_file(path, "/tmp", bytes, capacity, verify, BACKEND_DIRECT, depth, device, out);
}

mi_status mi_ssd_raw_uring_backend_create(const char *path, uint64_t offset, size_t capacity,
                                          size_t bytes, mi_ssd_verify verify, unsigned depth,
                                          int device, unsigned io_cpu_plus_one, mi_storage_backend **out) {
    if (!out || !path || !bytes || offset%4096 || capacity%4096 || !depth || depth>128 ||
        (unsigned)verify>MI_SSD_VERIFY_READ_BACK || offset>INT64_MAX || capacity>INT64_MAX-offset)
        return MI_EINVAL;
    *out=NULL;
#ifndef MINIINFER_WITH_URING
    (void)device; (void)io_cpu_plus_one;
    return MI_EUNSUPPORTED;
#else
    size_t stride;
    mi_status st=ssd_layout(bytes,capacity,MI_RECORD_HEADER,4096,&stride);
    if(st!=MI_OK) return st;
    int fd=-1;
    for(unsigned retry=0;retry<200;++retry) {
        fd=open(path,O_RDWR|O_DIRECT|O_EXCL|O_CLOEXEC);
        if(fd>=0 || errno!=EBUSY) break;
        usleep(10000);
    }
    if(fd<0) return MI_EIO;
    struct stat sb; uint64_t size=0;
    if(fstat(fd,&sb) || !S_ISBLK(sb.st_mode) || ioctl(fd,BLKGETSIZE64,&size) ||
        offset>size || capacity>size-offset) { close(fd); return MI_EINVAL; }
    mi_storage_backend *b;
    st=backend_create(BACKEND_DIRECT,MI_LOCATION_SSD,bytes,stride,capacity/stride,&b);
    if(st!=MI_OK) { close(fd); return st; }
    b->ssd.fd=fd; b->ssd.base_offset=offset; b->ssd.verify=verify; b->device=device;
    st=mi_direct_create_ex(fd,device,stride,depth,io_cpu_plus_one,&b->ssd.direct);
    if(st==MI_OK && strcmp(mi_direct_mode(b->ssd.direct),"direct-uring-fixed")) st=MI_EUNSUPPORTED;
    if(st!=MI_OK) { mi_backend_destroy(b); return st; }
    *out=b; return MI_OK;
#endif
}

const char *mi_ssd_file_io_name(const mi_storage_backend *b) {
    switch (b->kind) {
    case BACKEND_DIRECT: return mi_direct_mode(b->ssd.direct);
    case BACKEND_URING: return "buffered-uring";
    case BACKEND_POSIX: return "buffered-posix";
    default: return "not-file";
    }
}

mi_status mi_ssd_spdk_backend_create_ex(mi_spdk_config cfg, mi_spdk_options options, size_t bytes, mi_ssd_verify verify,
                                     mi_storage_backend **out) {
    if (!out || !bytes || (unsigned)verify > MI_SSD_VERIFY_READ_BACK ||
        !cfg.json_config || !*cfg.json_config || !cfg.bdev_name || !*cfg.bdev_name ||
        cfg.offset_bytes % SPDK_SECTOR || cfg.capacity_bytes % SPDK_SECTOR)
        return MI_EINVAL;
    *out = NULL;
    size_t stride;
    mi_status st = ssd_layout(bytes, cfg.capacity_bytes, MI_RECORD_HEADER, SPDK_SECTOR, &stride);
    if (st != MI_OK)
        return st;
#ifdef MINIINFER_WITH_SPDK
    mi_storage_backend *b;
    st = backend_create(BACKEND_SPDK, MI_LOCATION_SSD, bytes, stride, cfg.capacity_bytes / stride, &b);
    if (st != MI_OK)
        return st;
    st = mi_spdk_open_ex(cfg, options, &b->ssd.spdk);
    if (st != MI_OK) {
        mi_backend_destroy(b);
        return st;
    }
    st = mi_segment_pool_create(-1, stride, options.buffer_count ? options.buffer_count :
            (options.queue_depth ? options.queue_depth : 32), 0, &b->ssd.records_pool);
    if (st != MI_OK) { mi_backend_destroy(b); return st; }
    b->ssd.verify = verify;
    *out = b;
    return MI_OK;
#else
    (void)options;
    return MI_EUNSUPPORTED;
#endif
}

mi_status mi_ssd_spdk_backend_create(mi_spdk_config cfg, size_t bytes, mi_ssd_verify verify,
                                     mi_storage_backend **out) {
    return mi_ssd_spdk_backend_create_ex(cfg, (mi_spdk_options){0}, bytes, verify, out);
}

mi_location mi_backend_location(const mi_storage_backend *b) {
    return b ? b->location : MI_LOCATION_REMOTE;
}

size_t mi_backend_slot_bytes(const mi_storage_backend *b) {
    return b ? b->slot_bytes : 0;
}

size_t mi_backend_allocation_bytes(const mi_storage_backend *b, size_t payload) {
    return b && payload <= b->slot_bytes ? b->stride : 0;
}

int mi_backend_device(const mi_storage_backend *b) {
    return b->device;
}

int mi_backend_is_direct(const mi_storage_backend *b) {
    return b && b->kind == BACKEND_DIRECT;
}

int mi_backend_gpu_pending(const mi_storage_backend *b) {
    return b && ((b->location == MI_LOCATION_GPU &&
        atomic_load_explicit(&b->gpu_pending, memory_order_acquire)) ||
        (b->kind == BACKEND_DIRECT && mi_direct_gpu_pending(b->ssd.direct)));
}

static mi_status gpu_transfer(mi_storage_backend *b, void *ptr, void *host,
                               size_t bytes, int write, mi_stream stream, mi_event event) {
    uint64_t started=mi_io_now_ns();
    mi_status st = write
        ? mi_device_copy_from_host(b->device, ptr, host, bytes, stream)
        : mi_device_copy_to_host(b->device, host, ptr, bytes, stream);
    int complete = 0;
    if (st == MI_OK && event) {
        st = mi_device_event_record(b->device, event, stream);
        if (st == MI_OK)
            st = mi_device_event_wait(b->device, event);
        complete = st == MI_OK;
    }
    if (!complete) {
        mi_status waited = mi_device_stream_wait(b->device, stream);
        complete = waited == MI_OK;
        if (st == MI_OK)
            st = waited;
    }
    if (!complete) {
        mi_event fence = NULL;
        mi_status fenced = mi_device_event_create(b->device, &fence);
        if (fenced == MI_OK)
            fenced = mi_device_event_record(b->device, fence, stream);
        if (fenced == MI_OK)
            fenced = mi_device_event_wait(b->device, fence);
        complete = fenced == MI_OK;
        if (fence)
            (void)mi_device_event_destroy(b->device, fence);
    }
    if (!complete)
        atomic_store_explicit(&b->gpu_pending, 1, memory_order_release);
    atomic_fetch_add_explicit(&b->metrics[write?4:3],mi_io_now_ns()-started,memory_order_relaxed);
    return st;
}

mi_status mi_backend_gpu_copy(mi_storage_backend *b, mi_storage_handle handle,
                               size_t offset, void *host, size_t bytes, int write,
                               mi_stream stream, mi_event event) {
    if (mi_backend_gpu_pending(b))
        return MI_EIO;
    mi_segment_pool *pool = b->memory.transfers;
    mi_segment *scratch = mi_segment_acquire(pool, 0, pool->count);
    if (!scratch)
        return MI_EBUSY;
    if (write)
        memcpy(scratch->data, host, bytes);
    if (!stream) { stream = scratch->stream; event = scratch->event; }
    mi_status st = gpu_transfer(b, mi_backend_reserved_ptr(b, handle) + offset,
                                scratch->data, bytes, write, stream, event);
    int complete = !mi_backend_gpu_pending(b);
    if (complete) {
        if (!write && st == MI_OK)
            memcpy(host, scratch->data, bytes);
        mi_segment_release(scratch);
    } else {
        mi_segment_quarantine(scratch);
        if (st == MI_OK)
            st = MI_EIO;
    }
    return st;
}

mi_status mi_backend_gpu_copy_owned(mi_storage_backend *b, mi_storage_handle handle,
                                     void *host, size_t bytes, int write) {
    if (mi_backend_gpu_pending(b)) return MI_EIO;
    mi_segment_pool *pool = b->memory.transfers;
    mi_segment *lane = mi_segment_acquire(pool, 0, pool->count);
    if (!lane) return MI_EBUSY;
    mi_status st = gpu_transfer(b, mi_backend_reserved_ptr(b, handle), host, bytes, write,
                                lane->stream, lane->event);
    if (mi_backend_gpu_pending(b)) mi_segment_quarantine(lane);
    else mi_segment_release(lane);
    return st;
}

mi_status mi_backend_attach(mi_storage_backend *b, size_t limit, size_t *slots) {
    pthread_mutex_lock(&b->mutex);
    if (b->attached || b->free_count != b->capacity) {
        pthread_mutex_unlock(&b->mutex);
        return MI_EBUSY;
    }
    b->attached = 1;
    b->limit = limit < b->capacity ? limit : b->capacity;
    *slots = b->limit;
    pthread_mutex_unlock(&b->mutex);
    return MI_OK;
}

void mi_backend_detach(mi_storage_backend *b) {
    pthread_mutex_lock(&b->mutex);
    b->attached = 0;
    b->limit = b->capacity;
    int destroy = b->destroy_requested;
    pthread_mutex_unlock(&b->mutex);
    if (destroy)
        mi_backend_destroy(b);
}

static size_t available(const mi_storage_backend *b) {
    return b->limit - (b->capacity - b->free_count);
}

size_t mi_backend_available(mi_storage_backend *b) {
    pthread_mutex_lock(&b->mutex);
    size_t count = mi_backend_gpu_pending(b) ||
        (b->kind == BACKEND_DIRECT && mi_direct_failed(b->ssd.direct)) ? 0 : available(b);
    pthread_mutex_unlock(&b->mutex);
    return count;
}

mi_status mi_backend_reserve(mi_storage_backend *b, size_t bytes, mi_storage_handle *out) {
    if (!b || !out || !bytes || bytes > b->slot_bytes)
        return MI_EINVAL;
    pthread_mutex_lock(&b->mutex);
    if (mi_backend_gpu_pending(b) || (b->kind == BACKEND_DIRECT && mi_direct_failed(b->ssd.direct))) {
        pthread_mutex_unlock(&b->mutex);
        return MI_EIO;
    }
    if (!available(b)) {
        pthread_mutex_unlock(&b->mutex);
        return MI_ENOMEM;
    }
    uint32_t i = b->free_slots[--b->free_count];
    storage_slot *slot = &b->slots[i];
    if (++slot->generation == 0)
        ++slot->generation;
    slot->used = 1;
    *out = ((uint64_t)slot->generation << 32) | (i + UINT64_C(1));
    pthread_mutex_unlock(&b->mutex);
    return MI_OK;
}

mi_status mi_backend_release(mi_storage_backend *b, mi_storage_handle handle) {
    if (!b)
        return MI_EINVAL;
    if (mi_backend_gpu_pending(b))
        return MI_EIO;
    pthread_mutex_lock(&b->mutex);
    if (!owns_slot(b, handle)) {
        pthread_mutex_unlock(&b->mutex);
        return MI_ENOENT;
    }
    size_t i = slot_index(handle);
    if (b->kind != BACKEND_MEMORY)
        b->ssd.records[i] = (ssd_slot){0};
    b->slots[i].used = 0;
    b->free_slots[b->free_count++] = (uint32_t)i;
    pthread_mutex_unlock(&b->mutex);
    return MI_OK;
}

unsigned char *mi_backend_reserved_ptr(const mi_storage_backend *b, mi_storage_handle handle) {
    return b->memory.base + slot_index(handle) * b->stride;
}

mi_status mi_backend_memory_view(mi_storage_backend *b, mi_storage_handle handle, mi_memory_view *out) {
    if (!b || !out)
        return MI_EINVAL;
    if (mi_backend_gpu_pending(b))
        return MI_EIO;
    pthread_mutex_lock(&b->mutex);
    int found = owns_slot(b, handle);
    if (found) {
        *out = (mi_memory_view){
            .data = b->kind == BACKEND_MEMORY ? mi_backend_reserved_ptr(b, handle) : NULL,
            .capacity = b->kind == BACKEND_MEMORY ? b->slot_bytes : 0,
            .location = b->location,
            .device_id = b->device,
        };
    }
    pthread_mutex_unlock(&b->mutex);
    return found ? MI_OK : MI_ENOENT;
}

/* 按后端类型直调定位 I/O 实现。 */
static mi_status ssd_dispatch(mi_storage_backend *b, int write, mi_io_range range) {
    switch (b->kind) {
    case BACKEND_POSIX:
        return mi_posix_transfer(b->ssd.fd, write, range);
#ifdef MINIINFER_WITH_URING
    case BACKEND_URING:
        return mi_uring_transfer(b->ssd.ring, write, range);
#endif
#ifdef MINIINFER_WITH_SPDK
    case BACKEND_SPDK:
        return mi_spdk_transfer(b->ssd.spdk, write, range);
#endif
    default:
        return MI_EUNSUPPORTED;
    }
}

static mi_status ssd_transfer(mi_storage_backend *b, int write, mi_io_range range) {
    uint64_t started=mi_io_now_ns();
    mi_status st=ssd_dispatch(b,write,range);
    atomic_fetch_add_explicit(&b->metrics[0],mi_io_now_ns()-started,memory_order_relaxed);
    return st;
}

/* 校验槽内范围。内存走拷贝，SSD 走定位传输。 */
static mi_status data_transfer(mi_storage_backend *b, mi_storage_handle handle,
                                size_t offset, void *data, size_t bytes, int write) {
    if (!b || (bytes && !data))
        return MI_EINVAL;
    if (b->kind == BACKEND_DIRECT)
        return MI_EUNSUPPORTED;
    pthread_mutex_lock(&b->mutex);
    if (!owns_slot(b, handle)) {
        pthread_mutex_unlock(&b->mutex);
        return MI_ENOENT;
    }
    size_t cap = b->slot_bytes;
    ssd_slot *record = NULL;
    if (b->kind != BACKEND_MEMORY) {
        record = &b->ssd.records[slot_index(handle)];
        if (!!record->sealed_bytes == !!write) {
            pthread_mutex_unlock(&b->mutex);
            return MI_ESTATE;
        }
        if (!write)
            cap = record->sealed_bytes;
    }
    pthread_mutex_unlock(&b->mutex);
    if (offset > cap || bytes > cap - offset)
        return MI_EINVAL;
    if (!bytes)
        return MI_OK;
    if (record) {
        uint64_t base = slot_index(handle) * b->stride + (b->kind == BACKEND_SPDK ? MI_RECORD_HEADER : HEADER_BYTES);
        mi_status st = ssd_transfer(b, write, (mi_io_range){.data = data, .data_bytes = bytes,
                                                             .offset = base + offset});
        if (st == MI_OK && write) {
            pthread_mutex_lock(&b->mutex);
            if (offset + bytes > record->written)
                record->written = offset + bytes;
            pthread_mutex_unlock(&b->mutex);
        }
        return st;
    }

    unsigned char *ptr = mi_backend_reserved_ptr(b, handle);
    ptr += offset;
    if (b->location == MI_LOCATION_CPU) {
        if (write)
            memmove(ptr, data, bytes);
        else
            memmove(data, ptr, bytes);
        return MI_OK;
    }
    return mi_backend_gpu_copy(b, handle, offset, data, bytes, write, NULL, NULL);
}

mi_status mi_backend_read(mi_storage_backend *b, mi_storage_handle handle, size_t offset,
                          void *data, size_t bytes) {
    return data_transfer(b, handle, offset, data, bytes, 0);
}

mi_status mi_backend_write(mi_storage_backend *b, mi_storage_handle handle, size_t offset,
                           const void *data, size_t bytes) {
    return data_transfer(b, handle, offset, (void *)data, bytes, 1);
}

/* Preserve the 64-byte, little-endian MIFSSD1 record format. */
static void put_le(unsigned char *p, unsigned n, uint64_t value) {
    for (unsigned i = 0; i < n; ++i)
        p[i] = (unsigned char)(value >> (8 * i));
}

static uint64_t get_le(const unsigned char *p, unsigned n) {
    uint64_t value = 0;
    for (unsigned i = 0; i < n; ++i)
        value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static void encode(unsigned char header[HEADER_BYTES], const mi_block_meta *m, uint32_t crc) {
    memset(header, 0, HEADER_BYTES);
    memcpy(header, ssd_magic, sizeof(ssd_magic));
    put_le(header + 8, 4, 1);
    put_le(header + 12, 4, HEADER_BYTES);
    put_le(header + 16, 8, m->id);
    put_le(header + 24, 8, m->version);
    put_le(header + 32, 8, m->size);
    put_le(header + 40, 4, crc);
    put_le(header + 44, 4, m->location);
}

/* 分段读取 SSD 数据，增量计算 CRC32C。 */
static mi_status checksum_payload(mi_storage_backend *b, uint64_t offset, size_t bytes, uint32_t *out) {
    unsigned char buf[16384];
    uint32_t crc = 0;
    while (bytes) {
        size_t len = bytes < sizeof(buf) ? bytes : sizeof(buf);
        mi_status st = ssd_transfer(b, 0, (mi_io_range){.data = buf, .data_bytes = len, .offset = offset});
        if (st != MI_OK)
            return st;
        crc = mi_crc32c(crc, buf, len);
        bytes -= len;
        offset += len;
    }
    *out = crc;
    return MI_OK;
}

/* Caller owns the disk slot. Buffer pool bounds staging without per-I/O allocation. */
static mi_status aligned_record(mi_storage_backend *b, mi_storage_handle handle,
                                 const mi_block_meta *m, void *data, int write) {
    /* Full aligned Pages gather/scatter directly between the source/destination
     * and their request-owned DMA slot. No full-Page intermediate memcpy.
     * Keep padded/legacy publication and strict read-back on the pooled path. */
    if (data && m->size%4096==0 && m->size+MI_RECORD_HEADER==b->stride &&
        (!write || b->ssd.verify!=MI_SSD_VERIFY_READ_BACK)) {
        unsigned char header[MI_RECORD_HEADER];
        if (write) {
            uint64_t t=mi_io_now_ns();
            mi_record_encode(header,m,mi_crc32c(0,data,m->size));
            atomic_fetch_add_explicit(&b->metrics[1],mi_io_now_ns()-t,memory_order_relaxed);
        }
        mi_status st=ssd_transfer(b,write,(mi_io_range){header,sizeof(header),data,m->size,
                                                        slot_index(handle)*b->stride});
        if (st==MI_OK && !write) {
            uint64_t t=mi_io_now_ns();
            if(!mi_record_valid_payload(header,data,m,b->ssd.verify)) st=MI_ECRC;
            atomic_fetch_add_explicit(&b->metrics[1],mi_io_now_ns()-t,memory_order_relaxed);
        }
        return st;
    }
    mi_segment *slot;
    pthread_mutex_lock(&b->mutex);
    while (!(slot=mi_segment_acquire(b->ssd.records_pool,0,b->ssd.records_pool->count)))
        pthread_cond_wait(&b->io_changed,&b->mutex);
    pthread_mutex_unlock(&b->mutex);
    unsigned char *p = slot->data, *payload = p + MI_RECORD_HEADER;
    uint64_t base = slot_index(handle) * b->stride;
    mi_io_range range = {.data=p, .data_bytes=b->stride, .offset=base};
    mi_status st = MI_OK;
    if (write) {
        if (data) {
            uint64_t t=mi_io_now_ns(); memcpy(payload,data,m->size);
            atomic_fetch_add_explicit(&b->metrics[2],mi_io_now_ns()-t,memory_order_relaxed);
        }
        else st = ssd_transfer(b, 0, range); /* legacy partial-write publication */
        if (st == MI_OK) {
            memset(payload+m->size, 0, b->stride-MI_RECORD_HEADER-m->size);
            uint64_t t=mi_io_now_ns();
            mi_record_encode(p, m, mi_crc32c(0,payload,m->size));
            atomic_fetch_add_explicit(&b->metrics[1],mi_io_now_ns()-t,memory_order_relaxed);
            st = ssd_transfer(b, 1, range);
        }
    }
    if (st == MI_OK && (!write || b->ssd.verify == MI_SSD_VERIFY_READ_BACK)) {
        st = ssd_transfer(b, 0, range);
        uint64_t t=mi_io_now_ns();
        if (st == MI_OK && !mi_record_valid(p,m,b->ssd.verify)) st = MI_ECRC;
        atomic_fetch_add_explicit(&b->metrics[1],mi_io_now_ns()-t,memory_order_relaxed);
    }
    if (st == MI_OK && !write) {
        uint64_t t=mi_io_now_ns(); memcpy(data,payload,m->size);
        atomic_fetch_add_explicit(&b->metrics[2],mi_io_now_ns()-t,memory_order_relaxed);
    }
    pthread_mutex_lock(&b->mutex);
    mi_segment_release(slot);
    pthread_cond_signal(&b->io_changed);
    pthread_mutex_unlock(&b->mutex);
    return st;
}

static mi_status publish(mi_storage_backend *b, mi_storage_handle handle,
                          const mi_block_meta *m, const void *data) {
    pthread_mutex_lock(&b->mutex);
    if (!owns_slot(b, handle)) {
        pthread_mutex_unlock(&b->mutex);
        return MI_ENOENT;
    }
    ssd_slot *record = &b->ssd.records[slot_index(handle)];
    int blocked = record->sealed_bytes || (!data && record->written < m->size);
    pthread_mutex_unlock(&b->mutex);
    if (blocked)
        return MI_ESTATE;

    if (b->kind == BACKEND_SPDK) {
        mi_status st = aligned_record(b, handle, m, (void *)data, 1);
        if (st == MI_OK) {
            pthread_mutex_lock(&b->mutex);
            record->sealed_bytes = m->size;
            pthread_mutex_unlock(&b->mutex);
        }
        return st;
    }
    uint64_t base = slot_index(handle) * b->stride;
    uint32_t crc;
    mi_status st = MI_OK;
    if (data)
        crc = mi_crc32c(0, data, m->size);
    else
        st = checksum_payload(b, base + HEADER_BYTES, m->size, &crc);
    if (st != MI_OK)
        return st;

    unsigned char header[HEADER_BYTES];
    encode(header, m, crc);
    st = ssd_transfer(b, 1, (mi_io_range){header, sizeof(header), (void *)data, data ? m->size : 0, base});
    if (st != MI_OK)
        return st;
    if (b->ssd.verify == MI_SSD_VERIFY_READ_BACK) {
        uint32_t actual;
        st = checksum_payload(b, base + HEADER_BYTES, m->size, &actual);
        if (st != MI_OK || actual != crc)
            return st != MI_OK ? st : MI_ECRC;
    }
    pthread_mutex_lock(&b->mutex);
    /* 写入及校验成功后才公布可读长度。 */
    record->sealed_bytes = m->size;
    pthread_mutex_unlock(&b->mutex);
    return MI_OK;
}

mi_status mi_backend_publish(mi_storage_backend *b, mi_storage_handle handle, const mi_block_meta *m) {
    if (!b || !m || !m->id || !m->size || m->size > b->slot_bytes)
        return MI_EINVAL;
    if (b->kind == BACKEND_DIRECT)
        return MI_EUNSUPPORTED;
    if (b->kind == BACKEND_MEMORY) {
        pthread_mutex_lock(&b->mutex);
        int found = owns_slot(b, handle);
        pthread_mutex_unlock(&b->mutex);
        return found ? MI_OK : MI_ENOENT;
    }
    return publish(b, handle, m, NULL);
}

mi_status mi_backend_write_record(mi_storage_backend *b, mi_storage_handle handle,
                                  const mi_block_meta *m, const void *data) {
    if (!b || b->kind == BACKEND_MEMORY || !m || !data || !m->id || !m->size || m->size > b->slot_bytes)
        return MI_EINVAL;
    if (b->kind == BACKEND_DIRECT)
        return mi_backend_direct_move(b, handle, m, (void *)data, -1, 1);
    return publish(b, handle, m, data);
}

mi_status mi_backend_read_validated(mi_storage_backend *b, mi_storage_handle handle,
                                    const mi_block_meta *m, void *data) {
    if (!b || !m || !data || !m->size || m->size > b->slot_bytes)
        return MI_EINVAL;
    if (b->kind == BACKEND_MEMORY)
        return mi_backend_read(b, handle, 0, data, m->size);
    if (b->kind == BACKEND_DIRECT)
        return mi_backend_direct_move(b, handle, m, data, -1, 0);

    pthread_mutex_lock(&b->mutex);
    if (!owns_slot(b, handle)) {
        pthread_mutex_unlock(&b->mutex);
        return MI_ENOENT;
    }
    size_t sealed = b->ssd.records[slot_index(handle)].sealed_bytes;
    pthread_mutex_unlock(&b->mutex);
    if (!sealed)
        return MI_ESTATE;
    if (sealed != m->size)
        return MI_ECRC;

    if (b->kind == BACKEND_SPDK) return aligned_record(b, handle, m, data, 0);
    unsigned char header[HEADER_BYTES];
    uint64_t base = slot_index(handle) * b->stride;
    mi_status st = ssd_transfer(b, 0, (mi_io_range){header, sizeof(header), data, m->size, base});
    if (st != MI_OK)
        return st;
    if (memcmp(header, ssd_magic, sizeof(ssd_magic)) || get_le(header + 8, 4) != 1 ||
        get_le(header + 12, 4) != HEADER_BYTES || get_le(header + 16, 8) != m->id ||
        get_le(header + 24, 8) != m->version || get_le(header + 32, 8) != m->size ||
        (b->ssd.verify != MI_SSD_VERIFY_NONE && mi_crc32c(0, data, m->size) != get_le(header + 40, 4)))
        return MI_ECRC;
    return MI_OK;
}

mi_status mi_backend_direct_move(mi_storage_backend *b, mi_storage_handle handle,
                                  const mi_block_meta *m, void *data, int device, int write) {
    pthread_mutex_lock(&b->mutex);
    if (!owns_slot(b, handle)) {
        pthread_mutex_unlock(&b->mutex);
        return MI_ENOENT;
    }
    ssd_slot *record = &b->ssd.records[slot_index(handle)];
    size_t sealed = record->sealed_bytes;
    pthread_mutex_unlock(&b->mutex);
    if (write ? sealed != 0 : sealed != m->size)
        return MI_ESTATE;
    mi_status st = mi_direct_record(b->ssd.direct, write, b->ssd.base_offset + slot_index(handle) * b->stride,
                                    m, data, device, b->ssd.verify);
    if (st == MI_OK && write) {
        pthread_mutex_lock(&b->mutex);
        /* A partial write or failed read-back can never publish a readable slot. */
        record->sealed_bytes = m->size;
        pthread_mutex_unlock(&b->mutex);
    }
    return st;
}

void mi_backend_destroy(mi_storage_backend *b) {
    if (!b)
        return;
    pthread_mutex_lock(&b->mutex);
    if (b->attached) {
        b->destroy_requested = 1;
        pthread_mutex_unlock(&b->mutex);
        return;
    }
    pthread_mutex_unlock(&b->mutex);
    if (mi_backend_gpu_pending(b)) {
        pthread_mutex_lock(&retired_mutex);
        if (!b->retired) {
            b->retired = 1;
            b->retired_next = retired_backends;
            retired_backends = b;
        }
        pthread_mutex_unlock(&retired_mutex);
        return;
    }
    if (b->kind == BACKEND_MEMORY) {
        mi_segment_pool_destroy(b->memory.transfers);
        if (b->memory.base)
            (void)mi_mempool_release(b->memory.pool, b->memory.base);
        if (b->memory.pool)
            mi_mempool_drop(b->memory.pool);
    } else {
        mi_segment_pool_destroy(b->ssd.records_pool);
        mi_direct_destroy(b->ssd.direct);
#ifdef MINIINFER_WITH_URING
        if (b->ssd.ring)
            mi_uring_destroy(b->ssd.ring);
#endif
#ifdef MINIINFER_WITH_SPDK
        if (b->ssd.spdk)
            mi_spdk_close(b->ssd.spdk);
#endif
        if (b->ssd.fd >= 0)
            close(b->ssd.fd);
        free(b->ssd.records);
    }
    pthread_cond_destroy(&b->io_changed);
    pthread_mutex_destroy(&b->mutex);
    free(b->slots);
    free(b->free_slots);
    free(b);
}

void mi_backend_get_metrics(mi_storage_backend *b, mi_storage_metrics *out) {
    if(!b || !out) return;
    if(b->kind==BACKEND_DIRECT) { mi_direct_get_metrics(b->ssd.direct,out); return; }
    uint64_t v[5];
    for(unsigned i=0;i<5;++i) v[i]=atomic_load_explicit(&b->metrics[i],memory_order_relaxed);
#ifdef MINIINFER_WITH_SPDK
    if(b->kind==BACKEND_SPDK) { mi_spdk_stats stats; mi_spdk_get_stats(b->ssd.spdk,&stats); v[2]+=stats.copy_ns; }
#endif
    *out=(mi_storage_metrics){v[0],v[1],v[2],v[3],v[4]};
}
