/*
 * 定位 I/O 层。提供带短读短写处理的 POSIX 传输。
 * 描述符与缓冲区均由调用方持有。调用须在元数据锁外。
 */
#ifndef MINIINFER_FILE_IO_H
#define MINIINFER_FILE_IO_H
#include "common.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct mi_file_io_request {
    /* 读取目标或写入来源缓冲区。 */
    void *data;
    /* 要传输的字节数。 */
    size_t bytes;
    /* 文件起点算起的字节偏移。 */
    uint64_t offset;
} mi_file_io_request;

/* 从给定偏移完整读取指定字节数。 */
mi_status mi_file_read(int fd, void *dst, size_t bytes, uint64_t offset);
/* 从给定偏移完整写入指定字节数。 */
mi_status mi_file_write(int fd, const void *src, size_t bytes, uint64_t offset);

/* 按顺序执行定位读，遇到第一个错误停止。 */
mi_status mi_file_read_many(int fd, const mi_file_io_request *requests, size_t count);
/* 按顺序执行定位写，遇到第一个错误停止。 */
mi_status mi_file_write_many(int fd, const mi_file_io_request *requests, size_t count);
#ifdef __cplusplus
}
#endif
#endif
