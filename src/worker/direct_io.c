/*
 * SSD 文件层。对齐头和负载用同一个 Segment 发 I/O。块身份和双 CRC 校验
 * 将截断、撕裂或旧槽数据变成缓存未命中。每个槽有独立 Ring，允许并发提交。
 * 锁顺序：借 Segment 后才访问 Ring，Storage/Backend 元数据锁均在 I/O 前释放。
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "direct_io.h"
#include "io_transfer.h"
#include "segment_pool.h"
#include "storage_crc.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/vfs.h>
#include <unistd.h>

struct mi_direct_io {
    mi_segment_pool *pool;    /* 所有 Ring 的注册地址在此分配。 */
    int fd;                   /* 后端借出的文件描述符。 */
    int odirect;              /* 成功启用 O_DIRECT 后固定，直到关闭。 */
#ifdef MINIINFER_WITH_URING
    mi_uring *rings[64];      /* 每槽独占一 Ring，空表示定位 POSIX I/O。 */
#endif
};

static const unsigned char magic[8] = "MIFSSD2";

mi_status mi_direct_create(int fd, int device, size_t stride, unsigned depth, mi_direct_io **out) {
    struct statfs fs;
    if (fstatfs(fd, &fs) != 0)
        return MI_EIO;
    if (fs.f_type != 0xef53) {
        fprintf(stderr, "MiniInfer SSD: ext4 required for aligned file backend, statfs=0x%lx\n",
                (unsigned long)fs.f_type);
        return MI_EUNSUPPORTED;
    }
    mi_direct_io *io = calloc(1, sizeof(*io));
    if (!io)
        return MI_ENOMEM;
    io->fd = fd;
    mi_status st = mi_segment_pool_create(device, stride, depth, 1, &io->pool);
    if (st != MI_OK) {
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
        for (unsigned i = 0; i < depth; ++i) {
            st = mi_uring_create(fd, 2, &io->rings[i]);
            int error = st == MI_OK ? mi_uring_register_buffer(io->rings[i], io->pool->slots[i].data, stride) : 0;
            if (st == MI_OK && !error)
                continue;
            /* Keep one access mode for the entire file and all benchmark samples. */
            fprintf(stderr, "MiniInfer SSD: fixed buffers unavailable (%s), using O_DIRECT POSIX\n",
                    error < 0 ? strerror(-error) : mi_status_string(st));
            for (unsigned j = 0; j <= i; ++j) {
                mi_uring_destroy(io->rings[j]);
                io->rings[j] = NULL;
            }
            break;
        }
    }
#endif
    fprintf(stderr, "MiniInfer SSD: statfs=0xef53 mode=%s slots=%u\n", mi_direct_mode(io), depth);
    *out = io;
    return MI_OK;
}

const char *mi_direct_mode(const mi_direct_io *io) {
#ifdef MINIINFER_WITH_URING
    if (io->rings[0])
        return "direct-uring-fixed";
#endif
    return io->odirect ? "direct-posix" : "buffered-aligned";
}

void mi_direct_destroy(mi_direct_io *io) {
    if (!io)
        return;
#ifdef MINIINFER_WITH_URING
    for (unsigned i = 0; i < io->pool->count; ++i)
        mi_uring_destroy(io->rings[i]);
#endif
    mi_segment_pool_destroy(io->pool);
    free(io);
}

static void put(unsigned char *p, unsigned n, uint64_t value) {
    for (unsigned i = 0; i < n; ++i)
        p[n - i - 1] = (unsigned char)(value >> (8u * i));
}

static uint64_t get(const unsigned char *p, unsigned n) {
    uint64_t value = 0;
    for (unsigned i = 0; i < n; ++i)
        value = (value << 8) | p[i];
    return value;
}

static int valid_record(const unsigned char *header, const mi_block_meta *meta) {
    return !memcmp(header, magic, sizeof(magic)) &&
        get(header + 8, 8) == meta->id && get(header + 16, 8) == meta->version &&
        get(header + 24, 8) == meta->size && get(header + 32, 4) == (uint32_t)(meta->id >> 32) &&
        get(header + 40, 4) == mi_crc32c(0, header, 40) &&
        get(header + 36, 4) == mi_crc32c(0, header + MI_DIRECT_HEADER_BYTES, meta->size);
}

static mi_status record_io(mi_direct_io *io, mi_segment *slot, int write, uint64_t offset) {
    mi_io_range range = {.data = slot->data, .data_bytes = io->pool->stride, .offset = offset};
#ifdef MINIINFER_WITH_URING
    mi_uring *ring = io->rings[slot - io->pool->slots];
    if (ring)
        return mi_uring_transfer_fixed(ring, write, range);
#endif
    return io->odirect ? mi_posix_transfer_direct(io->fd, write, range)
                      : mi_posix_transfer(io->fd, write, range);
}

mi_status mi_direct_record(mi_direct_io *io, int write, uint64_t offset,
                            const mi_block_meta *meta, void *data, int device, mi_ssd_verify verify) {
    if (device >= 0 && device != io->pool->device)
        return MI_EINVAL;
    mi_segment *slot = mi_segment_acquire(io->pool, 0, io->pool->count);
    if (!slot)
        return MI_EBUSY;
    unsigned char *p = slot->data;
    unsigned char *payload = p + MI_DIRECT_HEADER_BYTES;
    mi_status st = MI_OK;
    if (write) {
        st = mi_device_copy_to_host(device, payload, data, meta->size, slot->stream);
        if (st == MI_OK && device >= 0)
            st = mi_device_stream_wait(device, slot->stream);
        if (st != MI_OK)
            goto done;
        memset(p, 0, MI_DIRECT_HEADER_BYTES);
        memset(payload + meta->size, 0, io->pool->stride - MI_DIRECT_HEADER_BYTES - meta->size);
        memcpy(p, magic, sizeof(magic));
        put(p + 8, 8, meta->id);
        put(p + 16, 8, meta->version);
        put(p + 24, 8, meta->size);
        put(p + 32, 4, meta->id >> 32);
        put(p + 36, 4, mi_crc32c(0, payload, meta->size));
        put(p + 40, 4, mi_crc32c(0, p, 40));
        st = record_io(io, slot, 1, offset);
        if (st != MI_OK || verify != MI_SSD_VERIFY_READ_BACK)
            goto done;
    }
    st = record_io(io, slot, 0, offset);
    if (st == MI_OK && !valid_record(p, meta))
        st = MI_ECRC;
    if (st == MI_OK && !write) {
        st = mi_device_copy_from_host(device, data, payload, meta->size, slot->stream);
        if (st == MI_OK && device >= 0)
            st = mi_device_stream_wait(device, slot->stream);
    }

done:
    /* Any submitted transfer has completed before making the DMA buffer reusable. */
    mi_segment_release(slot);
    return st;
}
