/*
 * 定位 I/O 层。提供带短读短写处理的 POSIX 传输。
 * 描述符与缓冲区均由调用方持有。调用须在元数据锁外。
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "miniinfer/file_io.h"
#include "io_transfer.h"
#include <errno.h>

/* 用定位向量 I/O 连续传输记录头和数据，处理短传输及 EINTR。 */
static mi_status transfer(int fd, int write, mi_io_range range, size_t alignment) {
    size_t total = range.header_bytes + range.data_bytes;
    for (size_t done = 0; done < total;) {
        struct iovec segments[2];
        size_t bytes;
        unsigned count = mi_io_segments(segments, range, done, 0x7ffff000, &bytes);
        off_t offset = (off_t)(range.offset + done);
        ssize_t n = write ? pwritev(fd, segments, count, offset) : preadv(fd, segments, count, offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || (size_t)n > bytes || ((size_t)n < bytes && (size_t)n % alignment))
            return MI_EIO;
        done += (size_t)n;
    }
    return MI_OK;
}

mi_status mi_posix_transfer(int fd, int write, mi_io_range range) {
    return transfer(fd, write, range, 1);
}

mi_status mi_posix_transfer_direct(int fd, int write, mi_io_range range) {
    return transfer(fd, write, range, 4096);
}

mi_status mi_file_read(int fd, void *dst, size_t bytes, uint64_t offset) {
    if (fd < 0 || (bytes && !dst) || offset > INT64_MAX || bytes > INT64_MAX - offset)
        return MI_EINVAL;
    return mi_posix_transfer(fd, 0, (mi_io_range){.data = dst, .data_bytes = bytes, .offset = offset});
}

mi_status mi_file_write(int fd, const void *src, size_t bytes, uint64_t offset) {
    if (fd < 0 || (bytes && !src) || offset > INT64_MAX || bytes > INT64_MAX - offset)
        return MI_EINVAL;
    return mi_posix_transfer(fd, 1,
                             (mi_io_range){.data = (void *)src, .data_bytes = bytes, .offset = offset});
}

mi_status mi_file_read_many(int fd, const mi_file_io_request *requests, size_t count) {
    if (count && !requests)
        return MI_EINVAL;
    for (size_t i = 0; i < count; ++i) {
        mi_status st = mi_file_read(fd, requests[i].data, requests[i].bytes, requests[i].offset);
        if (st != MI_OK)
            return st;
    }
    return MI_OK;
}

mi_status mi_file_write_many(int fd, const mi_file_io_request *requests, size_t count) {
    if (count && !requests)
        return MI_EINVAL;
    for (size_t i = 0; i < count; ++i) {
        mi_status st = mi_file_write(fd, requests[i].data, requests[i].bytes, requests[i].offset);
        if (st != MI_OK)
            return st;
    }
    return MI_OK;
}
