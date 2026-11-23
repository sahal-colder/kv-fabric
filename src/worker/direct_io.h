/*
 * 存储层的对齐文件记录 I/O。后端拥有 fd，本模块拥有注册的 Segment 和 Ring。
 * 调用方必须持有效的槽引用，不得持元数据锁跨过 I/O 或 DMA。
 */
#ifndef MINIINFER_DIRECT_IO_H
#define MINIINFER_DIRECT_IO_H

#include "miniinfer/storage_backend.h"

typedef struct mi_direct_io mi_direct_io;
enum { MI_DIRECT_HEADER_BYTES = 4096 };

mi_status mi_direct_create(int fd, int device, size_t stride, unsigned depth, mi_direct_io **out);
void mi_direct_destroy(mi_direct_io *io);
const char *mi_direct_mode(const mi_direct_io *io);
mi_status mi_direct_record(mi_direct_io *io, int write, uint64_t offset,
                            const mi_block_meta *meta, void *data, int device, mi_ssd_verify verify);

#endif
