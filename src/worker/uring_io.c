/*
 * Linux 文件 I/O 层。Ring 借用 fd。Fixed buffer 在 Ring 销毁前保持注册。
 * Ring 锁只保护提交和完成项。调用者已释放 Storage/后端元数据锁。
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "io_transfer.h"
#include <errno.h>
#include <liburing.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>

/* 每个文件后端有一个环，互斥锁串行化提交和完成处理。 */
struct mi_uring {
    struct io_uring ring;
    pthread_mutex_t mutex;
    int fd, failed;
};

mi_status mi_uring_create(int fd, unsigned depth, mi_uring **out) {
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
    *out = r;
    return MI_OK;
}

int mi_uring_register_buffer(mi_uring *r, void *data, size_t bytes) {
    struct iovec buf = {.iov_base = data, .iov_len = bytes};
    return io_uring_register_buffers(&r->ring, &buf, 1);
}

static mi_status transfer(mi_uring *r, int write, mi_io_range range, int fixed) {
    size_t total = range.header_bytes + range.data_bytes;
    mi_status status = MI_OK;
    pthread_mutex_lock(&r->mutex);
    for (size_t done = 0; done < total;) {
        if (r->failed) {
            status = MI_EIO;
            break;
        }
        struct iovec segments[2];
        size_t bytes;
        /* 按已完成字节数重新切片，支持短读写。 */
        unsigned count = mi_io_segments(segments, range, done, 0x7ffff000, &bytes);
        struct io_uring_sqe *sqe = io_uring_get_sqe(&r->ring);
        uint64_t offset = range.offset + done;
        if (fixed && write)
            io_uring_prep_write_fixed(sqe, r->fd, segments[0].iov_base, (unsigned)bytes, offset, 0);
        else if (fixed)
            io_uring_prep_read_fixed(sqe, r->fd, segments[0].iov_base, (unsigned)bytes, offset, 0);
        else if (write)
            io_uring_prep_writev(sqe, r->fd, segments, count, offset);
        else
            io_uring_prep_readv(sqe, r->fd, segments, count, offset);
        int result;
        do {
            result = io_uring_submit(&r->ring);
        } while (result == -EINTR);
        if (result != 1) {
            r->failed = 1;
            status = MI_EIO;
            break;
        }

        /* An accepted SQE retains these buffers until its CQE is consumed. */
        struct io_uring_cqe *cqe;
        do {
            result = io_uring_wait_cqe(&r->ring, &cqe);
        } while (result == -EINTR);
        if (result < 0) {
            status = MI_EIO;
            while (io_uring_peek_cqe(&r->ring, &cqe) != 0)
                sched_yield();
        }
        result = cqe->res;
        io_uring_cqe_seen(&r->ring, cqe);
        if (status != MI_OK)
            break;
        if (result == -EINTR)
            continue;
        /* Direct I/O may resume a short completion only at an aligned boundary. */
        if (result <= 0 || (size_t)result > bytes ||
            (fixed && (size_t)result < bytes && result % 4096)) {
            status = MI_EIO;
            break;
        }
        done += (size_t)result;
    }
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
    io_uring_queue_exit(&r->ring);
    pthread_mutex_destroy(&r->mutex);
    free(r);
}
