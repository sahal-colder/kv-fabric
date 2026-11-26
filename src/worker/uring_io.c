/* Ring-owned DMA buffers survive ambiguous submission/completion failures.
 * Ring mutex serializes its one in-flight operation; no metadata lock nests here. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "io_transfer.h"
#include "miniinfer/device_transfer.h"
#include <errno.h>
#include <liburing.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MI_URING_TEST
/* Compile-time seam: no Release branches or indirection. */
extern struct io_uring_sqe *mi_test_get_sqe(struct io_uring *);
extern int mi_test_wait_cqe(struct io_uring *, struct io_uring_cqe **);
extern int mi_test_peek_cqe(struct io_uring *, struct io_uring_cqe **);
#define io_uring_get_sqe mi_test_get_sqe
#define io_uring_wait_cqe mi_test_wait_cqe
#define io_uring_peek_cqe mi_test_peek_cqe
#endif

struct mi_uring {
    struct io_uring ring;
    pthread_mutex_t mutex;
    void *buffer, *allocation;
    size_t capacity;
    int fd, failed, fixed, inflight, device;
};

mi_status mi_uring_create(int fd, unsigned depth, mi_uring **out) {
    if (!out || fd < 0 || !depth || depth > 64)
        return MI_EINVAL;
    *out = NULL;
    mi_uring *r = calloc(1, sizeof(*r));
    if (!r)
        return MI_ENOMEM;
    if (pthread_mutex_init(&r->mutex, NULL) != 0) {
        free(r);
        return MI_EINTERNAL;
    }
    int err = io_uring_queue_init(depth, &r->ring, 0);
    if (err < 0) {
        pthread_mutex_destroy(&r->mutex);
        free(r);
        if (err == -ENOSYS || err == -ENOTSUP || err == -EPERM)
            return MI_EUNSUPPORTED;
        return err == -ENOMEM ? MI_ENOMEM : MI_EIO;
    }
    r->fd = fd;
    r->device = -1;
    *out = r;
    return MI_OK;
}

static int reserve_buffer(mi_uring *r, size_t bytes) {
    if (bytes <= r->capacity)
        return 0;
    if (r->fixed || r->inflight)
        return -EINVAL;
    void *buffer;
    int error = posix_memalign(&buffer, 4096, bytes);
    if (error)
        return -error;
    free(r->allocation);
    r->allocation = r->buffer = buffer;
    r->capacity = bytes;
    return 0;
}

int mi_uring_register_buffer(mi_uring *r, int device, size_t bytes) {
    if (!r || !bytes || bytes > SIZE_MAX - 4095 || r->buffer)
        return -EINVAL;
    void *allocation = NULL;
    mi_status st = mi_device_host_alloc(device, bytes + 4095, &allocation);
    if (st != MI_OK)
        return st == MI_ENOMEM ? -ENOMEM : -EIO;
    r->allocation = allocation;
    r->buffer = (char *)allocation + ((-(uintptr_t)allocation) & 4095);
    r->capacity = bytes;
    r->device = device;
    struct iovec buf = {.iov_base = r->buffer, .iov_len = bytes};
    int error = io_uring_register_buffers(&r->ring, &buf, 1);
    if (!error)
        r->fixed = 1;
    return error;
}

/* The Direct I/O slot has exclusive ownership of this ring until completion.
 * Returning NULL after a hard failure prevents mutation of quarantined bytes. */
void *mi_uring_buffer(mi_uring *r) {
    return r && r->fixed && !r->failed ? r->buffer : NULL;
}

/* Success (including ENOENT) establishes that no submitted request can touch
 * the owned buffer. If the kernel cannot establish this, retain the entire ring
 * at destroy rather than freeing memory still accessible by DMA. */
static int quiesce(mi_uring *r) {
    if (!r->inflight)
        return 1;
    struct io_uring_sync_cancel_reg cancel = {
        .flags = IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL,
        .timeout = {.tv_sec = 1}
    };
    int error = io_uring_register_sync_cancel(&r->ring, &cancel);
    if (error >= 0 || error == -ENOENT) {
        r->inflight = 0;
        return 1;
    }
    struct io_uring_cqe *cqe = NULL;
    if (!io_uring_peek_cqe(&r->ring, &cqe) && cqe) {
        io_uring_cqe_seen(&r->ring, cqe);
        r->inflight = 0;
        return 1;
    }
    return 0;
}

static mi_status transfer(mi_uring *r, int write, mi_io_range range, int fixed) {
    if (!r || range.header_bytes > SIZE_MAX - range.data_bytes ||
        (range.header_bytes && !range.header) || (range.data_bytes && !range.data))
        return MI_EINVAL;
    size_t total = range.header_bytes + range.data_bytes;
    if (range.offset > INT64_MAX || total > INT64_MAX - range.offset ||
        (fixed && (range.header_bytes || total % 4096 || range.offset % 4096)))
        return MI_EINVAL;
    pthread_mutex_lock(&r->mutex);
    mi_status status = MI_OK;
    if (r->failed || (fixed && !r->fixed)) {
        status = MI_EIO;
        goto done;
    }
    if (!total)
        goto done;
    int error = reserve_buffer(r, total);
    if (error) {
        status = error == -ENOMEM ? MI_ENOMEM : MI_EINVAL;
        goto done;
    }
    int own_buffer = fixed && range.data == r->buffer;
    if (write && !own_buffer) {
        if (range.header_bytes)
            memcpy(r->buffer, range.header, range.header_bytes);
        if (range.data_bytes)
            memcpy((char *)r->buffer + range.header_bytes, range.data, range.data_bytes);
    }
    unsigned completion_interrupts = 0;
    for (size_t done_bytes = 0; done_bytes < total;) {
        size_t bytes = total - done_bytes;
        if (bytes > 0x7ffff000)
            bytes = 0x7ffff000;
        struct io_uring_sqe *sqe = io_uring_get_sqe(&r->ring);
        if (!sqe) {
            r->failed = 1;
            status = MI_EIO;
            break;
        }
        void *data = (char *)r->buffer + done_bytes;
        uint64_t offset = range.offset + done_bytes;
        if (fixed && write)
            io_uring_prep_write_fixed(sqe, r->fd, data, (unsigned)bytes, offset, 0);
        else if (fixed)
            io_uring_prep_read_fixed(sqe, r->fd, data, (unsigned)bytes, offset, 0);
        else if (write)
            io_uring_prep_write(sqe, r->fd, data, (unsigned)bytes, offset);
        else
            io_uring_prep_read(sqe, r->fd, data, (unsigned)bytes, offset);
        r->inflight = 1; /* Conservatively includes ambiguous submit failure. */
        int result;
        unsigned interrupted = 0;
        do {
            result = io_uring_submit(&r->ring);
        } while (result == -EINTR && ++interrupted < 16);
        struct io_uring_cqe *cqe = NULL;
        if (result == 1) {
            interrupted = 0;
            do {
                result = io_uring_wait_cqe(&r->ring, &cqe);
            } while (result == -EINTR && ++interrupted < 16);
        } else {
            result = -EIO;
        }
        if (result < 0 || !cqe) {
            r->failed = 1;
            (void)quiesce(r);
            status = MI_EIO;
            break;
        }
        result = cqe->res;
        io_uring_cqe_seen(&r->ring, cqe);
        r->inflight = 0;
        if (result == -EINTR) {
            if (++completion_interrupts < 16)
                continue;
            status = MI_EIO;
            break;
        }
        completion_interrupts = 0;
        if (result <= 0 || (size_t)result > bytes ||
            (fixed && (size_t)result < bytes && result % 4096)) {
            status = MI_EIO;
            break;
        }
        done_bytes += (size_t)result;
    }
    if (status == MI_OK && !write && !own_buffer) {
        if (range.header_bytes)
            memcpy(range.header, r->buffer, range.header_bytes);
        if (range.data_bytes)
            memcpy(range.data, (char *)r->buffer + range.header_bytes, range.data_bytes);
    }
done:
    pthread_mutex_unlock(&r->mutex);
    return status;
}

mi_status mi_uring_transfer(mi_uring *r, int write, mi_io_range range) {
    return transfer(r, write, range, 0);
}
mi_status mi_uring_transfer_fixed(mi_uring *r, int write, mi_io_range range) {
    return transfer(r, write, range, 1);
}
void mi_uring_destroy(mi_uring *r) {
    if (!r)
        return;
    if (!quiesce(r)) {
        fprintf(stderr, "MiniInfer: quarantining io_uring with unconfirmed I/O completion\n");
        return;
    }
    io_uring_queue_exit(&r->ring);
    if (r->allocation)
        (void)mi_device_host_free(r->device, r->allocation);
    pthread_mutex_destroy(&r->mutex);
    free(r);
}
