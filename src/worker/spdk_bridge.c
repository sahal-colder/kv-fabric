/*
 * 可选 SSD I/O 层。管理 SPDK bdev 渠道和专用轮询线程。
 * 桥接对象持有设备和 DMA 资源。数据请求在 Storage 锁外提交。
 */
#define _POSIX_C_SOURCE 200809L
#include "spdk_bridge.h"
#include <errno.h>
#include <pthread.h>
#include <spdk/bdev.h>
#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/thread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { SECTOR = 4096, DMA_BYTES = 256 * 1024 };
enum { IO_READ, IO_WRITE, IO_READ_MODIFY };

/* reactor 持有块设备通道，通过条件变量向调用线程交回结果。 */
struct mi_spdk_device {
    mi_spdk_config config;
    struct spdk_bdev *bdev;
    struct spdk_bdev_desc *desc;
    struct spdk_io_channel *channel;
    struct spdk_thread *thread;
    struct spdk_poller *shutdown_poller;
    atomic_bool close_requested;
    struct spdk_bdev_io_wait_entry wait;

    pthread_t reactor;
    pthread_mutex_t submit, mutex;
    pthread_cond_t changed;
    int ready, done, removed, phase, write;
    mi_status status;

    unsigned char *dma;
    mi_io_range range;
    size_t position, skip, bytes, span;
    uint64_t offset;
};

static atomic_bool app_used;
static void issue(void *arg);
static void next_chunk(mi_spdk_device *d);

static void complete(mi_spdk_device *d, mi_status status) {
    pthread_mutex_lock(&d->mutex);
    d->status = status;
    d->done = 1;
    pthread_cond_signal(&d->changed);
    pthread_mutex_unlock(&d->mutex);
}

static void copy_dma(mi_spdk_device *d, int to_dma) {
    struct iovec v[2];
    size_t bytes;
    unsigned count = mi_io_segments(v, d->range, d->position, d->bytes, &bytes);
    unsigned char *dma = d->dma + d->skip;
    for (unsigned i = 0; i < count; ++i) {
        if (to_dma)
            memcpy(dma, v[i].iov_base, v[i].iov_len);
        else
            memcpy(v[i].iov_base, dma, v[i].iov_len);
        dma += v[i].iov_len;
    }
}

/* 完成读改写的读取阶段后发出写入，普通传输推进到下一段。 */
static void io_done(struct spdk_bdev_io *io, bool success, void *arg) {
    mi_spdk_device *d = arg;
    spdk_bdev_free_io(io);
    if (!success) {
        complete(d, MI_EIO);
        return;
    }
    if (d->phase == IO_READ_MODIFY) {
        copy_dma(d, 1);
        d->phase = IO_WRITE;
        issue(d);
        return;
    }
    if (d->phase == IO_READ)
        copy_dma(d, 0);
    d->position += d->bytes;
    next_chunk(d);
}

/* 提交当前片段，I/O 对象不足时等待 SPDK 通知重试。 */
static void issue(void *arg) {
    mi_spdk_device *d = arg;
    if (d->removed) {
        complete(d, MI_EIO);
        return;
    }
    int rc = d->phase == IO_WRITE
        ? spdk_bdev_write(d->desc, d->channel, d->dma, d->offset, d->span, io_done, d)
        : spdk_bdev_read(d->desc, d->channel, d->dma, d->offset, d->span, io_done, d);
    if (rc == -ENOMEM)
        rc = spdk_bdev_queue_io_wait(d->bdev, d->channel, &d->wait);
    if (rc)
        complete(d, MI_EIO);
}

/* Unaligned writes preserve the rest of each sector with a read-modify-write. */
static void next_chunk(mi_spdk_device *d) {
    size_t remaining = d->range.header_bytes + d->range.data_bytes - d->position;
    if (!remaining) {
        complete(d, MI_OK);
        return;
    }
    uint64_t address = d->config.offset_bytes + d->range.offset + d->position;
    /* 计算逻辑起点相对扇区边界的偏移。 */
    d->skip = address % SECTOR;
    d->offset = address - d->skip;
    d->bytes = remaining < DMA_BYTES - d->skip ? remaining : DMA_BYTES - d->skip;
    d->span = (d->skip + d->bytes + SECTOR - 1) & ~(size_t)(SECTOR - 1);
    d->phase = !d->write ? IO_READ
        : (d->skip || d->bytes != d->span ? IO_READ_MODIFY : IO_WRITE);
    if (d->phase == IO_WRITE)
        copy_dma(d, 1);
    issue(d);
}

static void submit_io(void *arg) {
    next_chunk(arg);
}

static void bdev_event(enum spdk_bdev_event_type event, struct spdk_bdev *bdev, void *arg) {
    (void)bdev;
    mi_spdk_device *d = arg;
    if (event == SPDK_BDEV_EVENT_REMOVE)
        d->removed = 1;
}

static void stop(void *arg) {
    mi_spdk_device *d = arg;
    if (d->channel)
        spdk_put_io_channel(d->channel);
    if (d->desc)
        spdk_bdev_close(d->desc);
    if (d->dma)
        spdk_dma_free(d->dma);
    spdk_app_stop(0);
}

/* Executed only by the owning SPDK thread, including when its message ring
 * cannot accept another message. No arbitrary-thread app_stop fallback. */
static int poll_shutdown(void *arg) {
    mi_spdk_device *d = arg;
    if (!atomic_load_explicit(&d->close_requested, memory_order_acquire))
        return SPDK_POLLER_IDLE;
    spdk_poller_unregister(&d->shutdown_poller);
    stop(d);
    return SPDK_POLLER_BUSY;
}

static void start(void *arg) {
    mi_spdk_device *d = arg;
    mi_status status = MI_EIO;
    if (spdk_bdev_open_ext(d->config.bdev_name, true, bdev_event, d, &d->desc))
        goto done;
    d->bdev = spdk_bdev_desc_get_bdev(d->desc);
    uint32_t sector = spdk_bdev_get_block_size(d->bdev);
    uint64_t blocks = spdk_bdev_get_num_blocks(d->bdev);
    status = MI_EINVAL;
    if (!sector || SECTOR % sector || blocks > UINT64_MAX / sector ||
        spdk_bdev_get_md_size(d->bdev) || spdk_bdev_get_buf_align(d->bdev) > SECTOR)
        goto done;
    uint64_t capacity = blocks * sector;
    if (d->config.offset_bytes > capacity || d->config.capacity_bytes > capacity - d->config.offset_bytes)
        goto done;
    struct spdk_bdev_opts opts = {0};
    spdk_bdev_get_opts(&opts, sizeof(opts));
    if (!opts.bdev_io_cache_size)
        goto done;

    status = MI_ENOMEM;
    d->channel = spdk_bdev_get_io_channel(d->desc);
    if (!d->channel)
        goto done;
    d->dma = spdk_dma_zmalloc(DMA_BYTES, SECTOR, NULL);
    if (!d->dma)
        goto done;
    d->thread = spdk_get_thread();
    d->shutdown_poller = spdk_poller_register(poll_shutdown, d, 1000);
    if (!d->shutdown_poller)
        goto done;
    d->wait = (struct spdk_bdev_io_wait_entry){.bdev = d->bdev, .cb_fn = issue, .cb_arg = d};
    status = MI_OK;

done:
    d->ready = 1;
    complete(d, status);
    if (status != MI_OK)
        stop(d);
}

static void *reactor(void *arg) {
    mi_spdk_device *d = arg;
    struct spdk_app_opts opts;
    spdk_app_opts_init(&opts, sizeof(opts));
    opts.name = "miniinfer_storage";
    opts.json_config_file = d->config.json_config;
    opts.reactor_mask = d->config.reactor_mask ? d->config.reactor_mask : "0x1";
    opts.rpc_addr = NULL;
    opts.disable_signal_handlers = true;
    opts.no_huge = d->config.no_huge;
    opts.no_pci = d->config.no_pci;
    opts.mem_size = 256;
    opts.num_entries = 0;
    if (!opts.no_huge)
        opts.env_context = "--legacy-mem";
    (void)spdk_app_start(&opts, start, d);
    spdk_app_fini();
    if (!d->ready)
        complete(d, MI_EIO);
    return NULL;
}

mi_status mi_spdk_open(mi_spdk_config config, mi_spdk_device **out) {
    if (!out || !config.json_config || !config.bdev_name || !config.capacity_bytes)
        return MI_EINVAL;
    *out = NULL;
    mi_spdk_device *d = calloc(1, sizeof(*d));
    if (!d)
        return MI_ENOMEM;
    d->config = config;
    atomic_init(&d->close_requested, false);
    mi_status status = MI_EINTERNAL;
    if (pthread_mutex_init(&d->submit, NULL))
        goto free_device;
    if (pthread_mutex_init(&d->mutex, NULL))
        goto free_submit;
    if (pthread_cond_init(&d->changed, NULL))
        goto free_mutex;

    /* The EAL app cannot migrate its main-thread state to a second reactor. */
    if (atomic_exchange(&app_used, true)) {
        status = MI_EBUSY;
        goto free_cond;
    }
    if (pthread_create(&d->reactor, NULL, reactor, d)) {
        atomic_store(&app_used, false);
        goto free_cond;
    }
    pthread_mutex_lock(&d->mutex);
    while (!d->done)
        pthread_cond_wait(&d->changed, &d->mutex);
    status = d->status;
    pthread_mutex_unlock(&d->mutex);
    if (status == MI_OK) {
        *out = d;
        return MI_OK;
    }
    pthread_join(d->reactor, NULL);
free_cond:
    pthread_cond_destroy(&d->changed);
free_mutex:
    pthread_mutex_destroy(&d->mutex);
free_submit:
    pthread_mutex_destroy(&d->submit);
free_device:
    free(d);
    return status;
}

mi_status mi_spdk_transfer(mi_spdk_device *d, int write, mi_io_range range) {
    pthread_mutex_lock(&d->submit);
    pthread_mutex_lock(&d->mutex);
    d->range = range;
    d->write = write;
    d->position = 0;
    d->done = 0;
    mi_status st = MI_EIO;
    if (spdk_thread_send_msg(d->thread, submit_io, d) == 0) {
        while (!d->done)
            pthread_cond_wait(&d->changed, &d->mutex);
        st = d->status;
    }
    pthread_mutex_unlock(&d->mutex);
    pthread_mutex_unlock(&d->submit);
    return st;
}

void mi_spdk_close(mi_spdk_device *d) {
    if (!d)
        return;
    pthread_mutex_lock(&d->submit);
    atomic_store_explicit(&d->close_requested, true, memory_order_release);
    pthread_join(d->reactor, NULL);
    pthread_mutex_unlock(&d->submit);
    pthread_cond_destroy(&d->changed);
    pthread_mutex_destroy(&d->mutex);
    pthread_mutex_destroy(&d->submit);
    free(d);
}
