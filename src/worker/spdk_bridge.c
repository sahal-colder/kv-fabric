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

enum { SECTOR = 4096, MAX_QD = 128 };
enum { FREE, QUEUED, ACTIVE, COMPLETE };
enum { IO_READ, IO_WRITE, IO_READ_MODIFY };
struct mi_spdk_request {
    mi_spdk_device *device;
    struct spdk_bdev_io_wait_entry wait;
    pthread_cond_t changed;
    atomic_bool cancelled;
    unsigned char *dma;
    mi_io_range range;
    mi_io_metrics metrics;
    size_t position, skip, bytes, span;
    uint64_t offset;
    int state, phase, write;
    mi_status status;
};
struct mi_spdk_device {
    mi_spdk_config config;
    mi_spdk_options options;
    struct spdk_bdev *bdev;
    struct spdk_bdev_desc *desc;
    struct spdk_io_channel *channel;
    struct spdk_poller *poller;
    pthread_t reactor;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    int ready, closing, removed;
    mi_status status;
    unsigned head, queued, owned, submitters;
    mi_spdk_request **queue, *requests;
    mi_spdk_stats stats;
};
static atomic_bool app_used;
static void issue(void *arg);
static void next_chunk(mi_spdk_request *r);

static void finish(mi_spdk_request *r, mi_status status) {
    mi_spdk_device *d = r->device;
    pthread_mutex_lock(&d->mutex);
    r->metrics.completed_ns = mi_io_now_ns();
    --d->stats.active_requests;
    ++d->stats.finished;
    d->stats.copy_ns += r->metrics.copy_ns;
    r->status = status;
    r->state = COMPLETE;
    pthread_cond_signal(&r->changed);
    pthread_cond_broadcast(&d->changed);
    pthread_mutex_unlock(&d->mutex);
}
static void copy_dma(mi_spdk_request *r, int to_dma) {
    uint64_t begin = mi_io_now_ns();
    struct iovec v[2];
    size_t bytes;
    unsigned count = mi_io_segments(v, r->range, r->position, r->bytes, &bytes);
    unsigned char *dma = r->dma + r->skip;
    for (unsigned i = 0; i < count; ++i) {
        if (to_dma) memcpy(dma, v[i].iov_base, v[i].iov_len);
        else memcpy(v[i].iov_base, dma, v[i].iov_len);
        dma += v[i].iov_len;
    }
    r->metrics.copy_ns += mi_io_now_ns() - begin;
}
static void io_done(struct spdk_bdev_io *io, bool success, void *arg) {
    mi_spdk_request *r = arg;
    mi_spdk_device *d = r->device;
    spdk_bdev_free_io(io);
    pthread_mutex_lock(&d->mutex);
    --d->stats.inflight;
    ++d->stats.completed;
    pthread_mutex_unlock(&d->mutex);
    if (!success || d->removed) { finish(r, MI_EIO); return; }
    if (atomic_load_explicit(&r->cancelled, memory_order_acquire)) {
        finish(r, MI_EBUSY); return;
    }
    if (r->phase == IO_READ_MODIFY) {
        copy_dma(r, 1);
        r->phase = IO_WRITE;
        issue(r);
        return;
    }
    if (r->phase == IO_READ) copy_dma(r, 0);
    r->position += r->bytes;
    next_chunk(r);
}
/* All calls and wait callbacks run on the channel's owning SPDK thread. */
static void issue(void *arg) {
    mi_spdk_request *r = arg;
    mi_spdk_device *d = r->device;
    if (d->removed) { finish(r, MI_EIO); return; }
    if (atomic_load_explicit(&r->cancelled, memory_order_acquire)) {
        finish(r, MI_EBUSY); return;
    }
    uint64_t begin = mi_io_now_ns();
    int rc = r->phase == IO_WRITE
        ? spdk_bdev_write(d->desc, d->channel, r->dma, r->offset, r->span, io_done, r)
        : spdk_bdev_read(d->desc, d->channel, r->dma, r->offset, r->span, io_done, r);
    if (!rc) {
        if (!r->metrics.started_ns) r->metrics.started_ns = begin;
        pthread_mutex_lock(&d->mutex);
        ++d->stats.submitted;
        if (++d->stats.inflight > d->stats.max_inflight)
            d->stats.max_inflight = d->stats.inflight;
        pthread_mutex_unlock(&d->mutex);
        return;
    }
    if (rc == -ENOMEM) {
        pthread_mutex_lock(&d->mutex);
        ++d->stats.pool_waits;
        pthread_mutex_unlock(&d->mutex);
        rc = spdk_bdev_queue_io_wait(d->bdev, d->channel, &r->wait);
        if (!rc) return; /* wait entry continues to own r and its DMA slot. */
    }
    finish(r, MI_EIO);
}
static void next_chunk(mi_spdk_request *r) {
    mi_spdk_device *d = r->device;
    size_t remaining = r->range.header_bytes + r->range.data_bytes - r->position;
    if (!remaining) { finish(r, MI_OK); return; }
    uint64_t address = d->config.offset_bytes + r->range.offset + r->position;
    r->skip = address % SECTOR;
    r->offset = address - r->skip;
    size_t limit = d->options.max_transfer_bytes - r->skip;
    r->bytes = remaining < limit ? remaining : limit;
    r->span = (r->skip + r->bytes + SECTOR - 1) & ~(size_t)(SECTOR - 1);
    r->phase = !r->write ? IO_READ :
        (r->skip || r->bytes != r->span ? IO_READ_MODIFY : IO_WRITE);
    if (r->phase == IO_WRITE) copy_dma(r, 1);
    issue(r);
}
static void bdev_event(enum spdk_bdev_event_type event, struct spdk_bdev *bdev, void *arg) {
    (void)bdev;
    mi_spdk_device *d = arg;
    if (event == SPDK_BDEV_EVENT_REMOVE) d->removed = 1;
}
static void stop(mi_spdk_device *d) {
    if (d->channel) spdk_put_io_channel(d->channel);
    if (d->desc) spdk_bdev_close(d->desc);
    for (unsigned i = 0; i < d->options.buffer_count; ++i)
        if (d->requests[i].dma) spdk_dma_free(d->requests[i].dma);
    spdk_app_stop(0);
}
static int poll_requests(void *arg) {
    mi_spdk_device *d = arg;
    unsigned issued = 0;
    for (; issued < d->options.submit_batch; ++issued) {
        pthread_mutex_lock(&d->mutex);
        if (!d->queued || d->stats.active_requests == d->options.queue_depth) {
            int shutdown = d->closing && !d->owned && !d->submitters;
            pthread_mutex_unlock(&d->mutex);
            if (shutdown) {
                spdk_poller_unregister(&d->poller);
                stop(d);
                return SPDK_POLLER_BUSY;
            }
            break;
        }
        mi_spdk_request *r = d->queue[d->head];
        d->head = (d->head + 1) % d->options.buffer_count;
        --d->queued;
        r->state = ACTIVE;
        if (++d->stats.active_requests > d->stats.max_active_requests)
            d->stats.max_active_requests = d->stats.active_requests;
        pthread_mutex_unlock(&d->mutex);
        next_chunk(r);
    }
    return issued ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}
static void ready(mi_spdk_device *d, mi_status st) {
    pthread_mutex_lock(&d->mutex);
    d->ready = 1;
    d->status = st;
    pthread_cond_broadcast(&d->changed);
    pthread_mutex_unlock(&d->mutex);
}
static void start(void *arg) {
    mi_spdk_device *d = arg;
    mi_status st = MI_EIO;
    if (spdk_bdev_open_ext(d->config.bdev_name, true, bdev_event, d, &d->desc)) goto done;
    d->bdev = spdk_bdev_desc_get_bdev(d->desc);
    uint32_t sector = spdk_bdev_get_block_size(d->bdev);
    uint64_t blocks = spdk_bdev_get_num_blocks(d->bdev);
    st = MI_EINVAL;
    if (!sector || SECTOR % sector || blocks > UINT64_MAX / sector ||
        spdk_bdev_get_md_size(d->bdev) || spdk_bdev_get_buf_align(d->bdev) > SECTOR) goto done;
    uint64_t capacity = blocks * sector;
    if (d->config.offset_bytes > capacity ||
        d->config.capacity_bytes > capacity - d->config.offset_bytes) goto done;
    struct spdk_bdev_opts opts = {0};
    spdk_bdev_get_opts(&opts, sizeof(opts));
    if (!opts.bdev_io_cache_size) goto done;
    st = MI_ENOMEM;
    d->channel = spdk_bdev_get_io_channel(d->desc);
    if (!d->channel) goto done;
    for (unsigned i = 0; i < d->options.buffer_count; ++i) {
        mi_spdk_request *r = &d->requests[i];
        r->dma = spdk_dma_zmalloc(d->options.max_transfer_bytes, SECTOR, NULL);
        if (!r->dma) goto done;
        r->wait = (struct spdk_bdev_io_wait_entry){.bdev=d->bdev, .cb_fn=issue, .cb_arg=r};
    }
    d->poller = spdk_poller_register(poll_requests, d, 0);
    if (!d->poller) goto done;
    st = MI_OK;
done:
    ready(d, st);
    if (st != MI_OK) stop(d);
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
    opts.iova_mode = opts.no_huge ? NULL : d->config.iova_mode;
    if (d->config.base_virtaddr) opts.base_virtaddr = d->config.base_virtaddr;
    size_t dma_mib = (d->options.buffer_count * d->options.max_transfer_bytes) / (1024 * 1024);
    opts.mem_size = (int)(256 + dma_mib);
    opts.num_entries = 0;
    if (!opts.no_huge) opts.env_context = "--legacy-mem";
    (void)spdk_app_start(&opts, start, d);
    spdk_app_fini();
    pthread_mutex_lock(&d->mutex);
    int initialized = d->ready;
    pthread_mutex_unlock(&d->mutex);
    if (!initialized) ready(d, MI_EIO);
    return NULL;
}
mi_status mi_spdk_open_ex(mi_spdk_config config, mi_spdk_options o, mi_spdk_device **out) {
    if (!out) return MI_EINVAL;
    *out = NULL;
    if (!config.json_config || !config.bdev_name || !config.capacity_bytes ||
        config.offset_bytes % SECTOR || config.capacity_bytes % SECTOR ||
        config.offset_bytes > UINT64_MAX - config.capacity_bytes) return MI_EINVAL;
    if (config.iova_mode && ((strcmp(config.iova_mode,"va") && strcmp(config.iova_mode,"pa")) ||
        (config.no_huge && !strcmp(config.iova_mode,"pa")))) return MI_EINVAL;
    if (!o.queue_depth) o.queue_depth = 32;
    if (!o.buffer_count) o.buffer_count = o.queue_depth;
    if (!o.submit_batch) o.submit_batch = o.queue_depth;
    if (!o.max_transfer_bytes) o.max_transfer_bytes = 1024 * 1024;
    if (o.queue_depth > MAX_QD || o.buffer_count < o.queue_depth || o.buffer_count > 512 ||
        o.submit_batch > MAX_QD || o.max_transfer_bytes % SECTOR ||
        o.max_transfer_bytes > 4 * 1024 * 1024) return MI_EINVAL;
    mi_spdk_device *d = calloc(1, sizeof(*d));
    if (!d) return MI_ENOMEM;
    d->config = config; d->options = o;
    d->requests = calloc(o.buffer_count, sizeof(*d->requests));
    d->queue = calloc(o.buffer_count, sizeof(*d->queue));
    mi_status st = MI_ENOMEM;
    unsigned initialized = 0;
    if (!d->requests || !d->queue) goto free_device;
    st = MI_EINTERNAL;
    if (pthread_mutex_init(&d->mutex, NULL)) goto free_device;
    if (pthread_cond_init(&d->changed, NULL)) goto free_mutex;
    for (; initialized < o.buffer_count; ++initialized) {
        mi_spdk_request *r = &d->requests[initialized];
        r->device = d; atomic_init(&r->cancelled, false);
        if (pthread_cond_init(&r->changed, NULL)) goto free_cond;
    }
    /* EAL can only be started once per process, including failed initialization. */
    if (atomic_exchange(&app_used, true)) { st = MI_EBUSY; goto free_cond; }
    if (pthread_create(&d->reactor, NULL, reactor, d)) {
        atomic_store(&app_used, false); goto free_cond;
    }
    pthread_mutex_lock(&d->mutex);
    while (!d->ready) pthread_cond_wait(&d->changed, &d->mutex);
    st = d->status;
    pthread_mutex_unlock(&d->mutex);
    if (st == MI_OK) { *out = d; return MI_OK; }
    pthread_join(d->reactor, NULL);
free_cond:
    for (unsigned i = 0; i < initialized; ++i) pthread_cond_destroy(&d->requests[i].changed);
    pthread_cond_destroy(&d->changed);
free_mutex:
    pthread_mutex_destroy(&d->mutex);
free_device:
    free(d->queue); free(d->requests); free(d);
    return st;
}
mi_status mi_spdk_open(mi_spdk_config c, mi_spdk_device **out) {
    return mi_spdk_open_ex(c, (mi_spdk_options){0}, out);
}
static mi_status submit(mi_spdk_device *d, int write, mi_io_range range, mi_spdk_request **out, int wait) {
    if (!d || !out) return MI_EINVAL;
    *out = NULL;
    if (range.header_bytes > SIZE_MAX - range.data_bytes ||
        (range.header_bytes && !range.header) || (range.data_bytes && !range.data)) return MI_EINVAL;
    size_t bytes = range.header_bytes + range.data_bytes;
    if (range.offset > d->config.capacity_bytes || bytes > d->config.capacity_bytes-range.offset)
        return MI_EINVAL;
    pthread_mutex_lock(&d->mutex);
    ++d->submitters;
    while (!d->closing && d->owned == d->options.buffer_count) {
        ++d->stats.queue_waits;
        if (!wait) { --d->submitters; pthread_mutex_unlock(&d->mutex); return MI_EBUSY; }
        pthread_cond_wait(&d->changed, &d->mutex);
    }
    if (d->closing) { --d->submitters; pthread_mutex_unlock(&d->mutex); return MI_ESTATE; }
    mi_spdk_request *r = d->requests;
    while (r->state != FREE) ++r;
    r->metrics = (mi_io_metrics){.queued_ns=mi_io_now_ns()};
    r->range = range; r->write = !!write; r->position = 0; r->status = MI_OK;
    atomic_store_explicit(&r->cancelled, false, memory_order_relaxed);
    r->state = QUEUED;
    d->queue[(d->head + d->queued++) % d->options.buffer_count] = r;
    ++d->owned; ++d->stats.requests; --d->submitters;
    *out = r;
    pthread_mutex_unlock(&d->mutex);
    return MI_OK;
}
mi_status mi_spdk_submit(mi_spdk_device *d, int write, mi_io_range range, mi_spdk_request **out) {
    return submit(d, write, range, out, 0);
}
mi_status mi_spdk_wait_metrics(mi_spdk_request *r, mi_io_metrics *metrics) {
    if (!r) return MI_EINVAL;
    mi_spdk_device *d = r->device;
    pthread_mutex_lock(&d->mutex);
    while (r->state != COMPLETE) pthread_cond_wait(&r->changed, &d->mutex);
    mi_status st = r->status;
    if (metrics) *metrics = r->metrics;
    r->state = FREE; --d->owned;
    pthread_cond_broadcast(&d->changed);
    pthread_mutex_unlock(&d->mutex);
    return st;
}
mi_status mi_spdk_wait(mi_spdk_request *r) { return mi_spdk_wait_metrics(r, NULL); }
void mi_spdk_cancel(mi_spdk_request *r) {
    if (r) atomic_store_explicit(&r->cancelled, true, memory_order_release);
}
mi_status mi_spdk_transfer(mi_spdk_device *d, int write, mi_io_range range) {
    mi_spdk_request *r;
    mi_status st = submit(d, write, range, &r, 1);
    return st == MI_OK ? mi_spdk_wait(r) : st;
}
mi_status mi_spdk_transfer_batch(mi_spdk_device *d, int write, const mi_io_range *ranges,
                                 size_t count, mi_status *results) {
    if (!d || !ranges || !results || !count) return MI_EINVAL;
    mi_status first = MI_OK;
    for (size_t base = 0; base < count;) {
        mi_spdk_request *pending[MAX_QD] = {0};
        size_t n = count-base;
        if (n > d->options.queue_depth) n = d->options.queue_depth;
        size_t accepted = 0;
        for (; accepted < n; ++accepted) {
            mi_status st = submit(d, write, ranges[base+accepted], &pending[accepted], !accepted);
            if (st == MI_EBUSY) break;
            results[base+accepted] = st;
        }
        n = accepted;
        for (size_t i = 0; i < n; ++i) {
            if (pending[i]) results[base+i] = mi_spdk_wait(pending[i]);
            if (first == MI_OK) first = results[base+i];
        }
        base += n;
    }
    return first;
}
void mi_spdk_get_stats(mi_spdk_device *d, mi_spdk_stats *out) {
    if (!d || !out) return;
    pthread_mutex_lock(&d->mutex); *out = d->stats; pthread_mutex_unlock(&d->mutex);
}
void mi_spdk_close(mi_spdk_device *d) {
    if (!d) return;
    pthread_mutex_lock(&d->mutex);
    d->closing = 1;
    pthread_cond_broadcast(&d->changed);
    pthread_mutex_unlock(&d->mutex);
    pthread_join(d->reactor, NULL);
    for (unsigned i = 0; i < d->options.buffer_count; ++i) pthread_cond_destroy(&d->requests[i].changed);
    pthread_cond_destroy(&d->changed); pthread_mutex_destroy(&d->mutex);
    free(d->queue); free(d->requests); free(d);
}
