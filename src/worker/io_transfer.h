/*
 * 存储层内部定位 I/O 接口。调用方借出缓冲区直到返回。
 * Ring 锁只串行化 SQ/CQ，不嵌套 Storage、后端槽或 Trie 锁。
 */
#ifndef MINIINFER_IO_TRANSFER_H
#define MINIINFER_IO_TRANSFER_H
#include "miniinfer/common.h"
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一次定位 I/O 的两段连续文件区间，先记录头后数据。 */
typedef struct mi_io_range {
    /* 可选的记录头缓冲区。 */
    void *header;
    /* 记录头长度，为 0 时跳过。 */
    size_t header_bytes;
    /* 数据缓冲区。 */
    void *data;
    /* 数据长度。 */
    size_t data_bytes;
    /* 记录在文件或设备区域中的起点。 */
    uint64_t offset;
} mi_io_range;

/* 使用 preadv 或 pwritev 完成定位传输。 */
mi_status mi_posix_transfer(int fd, int write, mi_io_range range);
mi_status mi_posix_transfer_direct(int fd, int write, mi_io_range range);

#ifdef MINIINFER_WITH_URING
typedef struct mi_uring mi_uring;
/* 创建关联到 fd 的 io_uring 队列。 */
mi_status mi_uring_create(int fd, unsigned depth, mi_uring **out);
/* 提交传输并等待完成项。 */
mi_status mi_uring_transfer(mi_uring *ring, int write, mi_io_range range);
/* The ring owns this CPU/CUDA-pinned fixed allocation, including on failure. */
int mi_uring_register_buffer(mi_uring *ring, int device, size_t bytes);
void *mi_uring_buffer(mi_uring *ring);
mi_status mi_uring_transfer_fixed(mi_uring *ring, int write, mi_io_range range);
/* 释放队列。文件描述符由后端关闭。 */
void mi_uring_destroy(mi_uring *ring);
#endif

#ifdef __cplusplus
}
#endif

/* 跳过已传输的字节，返回本次最多两段的 iovec 数。 */
static inline unsigned mi_io_segments(struct iovec v[2], mi_io_range range,
                                      size_t skip, size_t limit, size_t *bytes) {
    void *base[] = {range.header, range.data};
    size_t lengths[] = {range.header_bytes, range.data_bytes};
    unsigned count = 0;
    *bytes = 0;
    for (unsigned i = 0; i < 2 && limit; ++i) {
        if (skip >= lengths[i]) {
            /* 当前缓冲区已经传完，从下一段继续。 */
            skip -= lengths[i];
            continue;
        }
        size_t take = lengths[i] - skip;
        if (take > limit)
            take = limit;
        v[count].iov_base = (unsigned char *)base[i] + skip;
        /* 单个 iovec 最多占用当前剩余传输预算。 */
        v[count++].iov_len = take;
        *bytes += take;
        limit -= take;
        skip = 0;
    }
    return count;
}
#endif
