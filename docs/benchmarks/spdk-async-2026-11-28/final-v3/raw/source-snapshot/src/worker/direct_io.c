/*
 * SSD 文件层。对齐头和负载共用缓冲（fixed 模式由 Ring 拥有）。块身份与双 CRC
 * 将截断、撕裂或旧槽数据变成缓存未命中。整个后端共享多在途 Ring，批量收发；每个 GPU 槽保留独立 Stream/Event。
 * 锁顺序：借 Segment 后才访问 Ring，Storage/Backend 元数据锁均在 I/O 前释放。
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "direct_io.h"
#include "io_transfer.h"
#include "segment_pool.h"
#include "storage_record.h"
#ifdef MINIINFER_WITH_NVTX
#include <nvtx3/nvToolsExt.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/vfs.h>
#include <sys/stat.h>
#include <unistd.h>

struct mi_direct_io {
    pthread_mutex_t pool_mutex;
    pthread_cond_t pool_changed;
    mi_segment_pool *pool;    /* POSIX 回退缓冲与各槽的 Stream/Event。 */
    int fd;                   /* 后端借出的文件描述符。 */
    atomic_bool failed;        /* Hard I/O failure fences this file until reopen. */
    atomic_bool gpu_pending;
    struct mi_direct_io *retired_next;
    int retired;
    atomic_uint_fast64_t metrics[5];
    int odirect;              /* 成功启用 O_DIRECT 后固定，直到关闭。 */
#ifdef MINIINFER_WITH_URING
    mi_uring *ring;            /* 唯一 I/O owner，内部注册的多缓冲池。 */
#endif
};

static pthread_mutex_t retired_mutex = PTHREAD_MUTEX_INITIALIZER;
static mi_direct_io *retired_io;



mi_status mi_direct_create_ex(int fd, int device, size_t stride, unsigned depth, unsigned io_cpu_plus_one, mi_direct_io **out) {
    if (!out || fd < 0 || !depth || depth > 128 || stride < MI_DIRECT_HEADER_BYTES || stride % 4096)
        return MI_EINVAL;
    *out = NULL;
    struct statfs fs;
    if (fstatfs(fd, &fs) != 0)
        return MI_EIO;
    struct stat statbuf;
    if (fstat(fd, &statbuf)) return MI_EIO;
    if (fs.f_type != 0xef53 && !S_ISBLK(statbuf.st_mode)) {
        fprintf(stderr, "MiniInfer SSD: ext4 required for aligned file backend, statfs=0x%lx\n",
                (unsigned long)fs.f_type);
        return MI_EUNSUPPORTED;
    }
    mi_direct_io *io = calloc(1, sizeof(*io));
    if (!io)
        return MI_ENOMEM;
    if (pthread_mutex_init(&io->pool_mutex,NULL)) { free(io); return MI_EINTERNAL; }
    if (pthread_cond_init(&io->pool_changed,NULL)) {
        pthread_mutex_destroy(&io->pool_mutex); free(io); return MI_EINTERNAL;
    }
    io->fd = fd;
    for (unsigned i=0;i<5;++i) atomic_init(&io->metrics[i],0);
    atomic_init(&io->failed, 0);
    atomic_init(&io->gpu_pending, 0);
    mi_status st = mi_segment_pool_create(device, stride, depth, 1, &io->pool);
    if (st != MI_OK) {
        pthread_cond_destroy(&io->pool_changed); pthread_mutex_destroy(&io->pool_mutex);
        free(io);
        return st;
    }

    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0 && fcntl(fd, F_SETFL, flags | O_DIRECT) == 0)
        io->odirect = 1;
    else
        fprintf(stderr, "MiniInfer SSD: O_DIRECT unavailable (%s), using buffered POSIX\n", strerror(errno));
#ifdef MINIINFER_WITH_URING
    if (io->odirect) {
        st = mi_uring_create_ex(fd, (mi_uring_options){.queue_depth=depth, .buffer_bytes=stride,
                .fixed_buffers=1, .fixed_file=1, .worker_cpu_plus_one=io_cpu_plus_one}, &io->ring);
        if (st != MI_OK)
            fprintf(stderr, "MiniInfer SSD: fixed ring unavailable (%s), using O_DIRECT POSIX\n", mi_status_string(st));
    }
#else
    (void)io_cpu_plus_one;
#endif
    fprintf(stderr, "MiniInfer SSD: statfs=0xef53 mode=%s slots=%u\n", mi_direct_mode(io), depth);
    *out = io;
    return MI_OK;
}

mi_status mi_direct_create(int fd, int device, size_t stride, unsigned depth, mi_direct_io **out) {
    return mi_direct_create_ex(fd,device,stride,depth,0,out);
}
const char *mi_direct_mode(const mi_direct_io *io) {
#ifdef MINIINFER_WITH_URING
    if (io->ring)
        return "direct-uring-fixed";
#endif
    return io->odirect ? "direct-posix" : "buffered-aligned";
}

void mi_direct_destroy(mi_direct_io *io) {
    if (!io)
        return;
    if (mi_direct_gpu_pending(io)) {
        pthread_mutex_lock(&retired_mutex);
        if (!io->retired) {
            io->retired = 1;
            io->retired_next = retired_io;
            retired_io = io;
        }
        pthread_mutex_unlock(&retired_mutex);
        return;
    }
#ifdef MINIINFER_WITH_URING
    mi_uring_destroy(io->ring);
#endif
    mi_segment_pool_destroy(io->pool);
    pthread_cond_destroy(&io->pool_changed); pthread_mutex_destroy(&io->pool_mutex);
    free(io);
}

static mi_status record_io(mi_direct_io *io, mi_segment *slot, void *data, int write, uint64_t offset) {
    uint64_t started=mi_io_now_ns();
    mi_io_range range = {.data = data, .data_bytes = io->pool->stride, .offset = offset};
#ifdef MINIINFER_WITH_NVTX
    nvtxRangePushA(write ? "kvfabric.disk_io.write" : "kvfabric.disk_io.read");
#endif
#ifdef MINIINFER_WITH_URING
    (void)slot;
    mi_status st = io->ring ? mi_uring_transfer(io->ring, write, range) :
#else
    (void)slot;
    mi_status st =
#endif
        io->odirect ? mi_posix_transfer_direct(io->fd, write, range)
                    : mi_posix_transfer(io->fd, write, range);
#ifdef MINIINFER_WITH_NVTX
    nvtxRangePop();
#endif
    atomic_fetch_add_explicit(&io->metrics[0],mi_io_now_ns()-started,memory_order_relaxed);
    if (st == MI_EIO)
        atomic_store_explicit(&io->failed, 1, memory_order_release);
    return st;
}

int mi_direct_failed(const mi_direct_io *io) {
    return io && atomic_load_explicit(&io->failed, memory_order_acquire);
}

int mi_direct_gpu_pending(const mi_direct_io *io) {
    return io && atomic_load_explicit(&io->gpu_pending, memory_order_acquire);
}

/* Even a copy error may report an earlier asynchronous failure. Confirm the
 * exact stream before returning either its device address or host staging. */
static mi_status finish_copy(mi_direct_io *io, mi_segment *slot, int device, mi_status copy) {
    if (device < 0)
        return copy;
    mi_status waited = mi_device_stream_wait(device, slot->stream);
    if (waited != MI_OK) {
        mi_status fenced = mi_device_event_record(device, slot->event, slot->stream);
        if (fenced == MI_OK)
            fenced = mi_device_event_wait(device, slot->event);
        if (fenced != MI_OK) {
            atomic_store_explicit(&io->gpu_pending, 1, memory_order_release);
            atomic_store_explicit(&io->failed, 1, memory_order_release);
        }
    }
    return copy != MI_OK ? copy : waited;
}

mi_status mi_direct_record(mi_direct_io *io, int write, uint64_t offset,
                            const mi_block_meta *meta, void *data, int device, mi_ssd_verify verify) {
    if (!io || !meta || !data || !meta->size ||
        meta->size > io->pool->stride - MI_DIRECT_HEADER_BYTES || offset % 4096 ||
        offset > INT64_MAX || io->pool->stride > INT64_MAX - offset ||
        (unsigned)verify > MI_SSD_VERIFY_READ_BACK ||
        (device >= 0 && device != io->pool->device))
        return MI_EINVAL;
    if (mi_direct_failed(io))
        return MI_EIO;
#ifdef MINIINFER_WITH_NVTX
    nvtxRangePushA("kvfabric.disk_slot.acquire");
#endif
    mi_segment *slot=NULL;
    pthread_mutex_lock(&io->pool_mutex);
    while (!mi_direct_failed(io) && !(slot=mi_segment_acquire(io->pool,0,io->pool->count)))
        pthread_cond_wait(&io->pool_changed,&io->pool_mutex);
    pthread_mutex_unlock(&io->pool_mutex);
#ifdef MINIINFER_WITH_NVTX
    nvtxRangePop();
#endif
    if (!slot)
        return MI_EIO;
    unsigned char *p = slot->data;

    unsigned char *payload = p + MI_DIRECT_HEADER_BYTES;
    mi_status st = MI_OK;
    if (write) {
        uint64_t started=mi_io_now_ns();
        st = mi_device_copy_to_host(device, payload, data, meta->size, slot->stream);
        st = finish_copy(io, slot, device, st);
        atomic_fetch_add_explicit(&io->metrics[device<0?2:3],mi_io_now_ns()-started,memory_order_relaxed);
        if (st != MI_OK)
            goto done;
        memset(payload + meta->size, 0, io->pool->stride - MI_DIRECT_HEADER_BYTES - meta->size);
        started=mi_io_now_ns();
        mi_record_encode(p, meta, mi_crc32c(0, payload, meta->size));
        atomic_fetch_add_explicit(&io->metrics[1],mi_io_now_ns()-started,memory_order_relaxed);
        st = record_io(io, slot, p, 1, offset);
        if (st != MI_OK || verify != MI_SSD_VERIFY_READ_BACK)
            goto done;
    }
    st = record_io(io, slot, p, 0, offset);
#ifdef MINIINFER_WITH_NVTX
    nvtxRangePushA("kvfabric.disk_crc.read");
#endif
    uint64_t crc_started=mi_io_now_ns();
    if (st == MI_OK && !mi_record_valid(p, meta, verify))
        st = MI_ECRC;
    atomic_fetch_add_explicit(&io->metrics[1],mi_io_now_ns()-crc_started,memory_order_relaxed);
#ifdef MINIINFER_WITH_NVTX
    nvtxRangePop();
#endif
    if (st == MI_OK && !write) {
#ifdef MINIINFER_WITH_NVTX
        nvtxRangePushA("kvfabric.disk_stage.read");
#endif
        uint64_t started=mi_io_now_ns();
        st = mi_device_copy_from_host(device, data, payload, meta->size, slot->stream);
        st = finish_copy(io, slot, device, st);
        atomic_fetch_add_explicit(&io->metrics[device<0?2:4],mi_io_now_ns()-started,memory_order_relaxed);
#ifdef MINIINFER_WITH_NVTX
        nvtxRangePop();
#endif
    }

done:
    /* Any submitted transfer has completed before making the DMA buffer reusable. */
    pthread_mutex_lock(&io->pool_mutex);
    if (!mi_direct_gpu_pending(io))
        mi_segment_release(slot);
    pthread_cond_broadcast(&io->pool_changed);
    pthread_mutex_unlock(&io->pool_mutex);
    return st;
}

void mi_direct_get_metrics(mi_direct_io *io, mi_storage_metrics *out) {
    uint64_t v[5];
    for(unsigned i=0;i<5;++i) v[i]=atomic_load_explicit(&io->metrics[i],memory_order_relaxed);
#ifdef MINIINFER_WITH_URING
    if(io->ring) { mi_uring_stats stats; mi_uring_get_stats(io->ring,&stats); v[2]+=stats.copy_ns; }
#endif
    *out=(mi_storage_metrics){v[0],v[1],v[2],v[3],v[4]};
}
