/*
 * 设备层的旧 CUDA 入口。资源由调用方管理。
 * 接口调用应在 Storage/Trie 元数据锁外完成。
 */
#ifndef MINIINFER_CUDA_ADAPTER_H
#define MINIINFER_CUDA_ADAPTER_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 将设备数据复制到主机缓冲区，NULL 使用默认流。 */
mi_status mi_cuda_copy_to_host(int device, void *host, const void *src,
                               size_t bytes, mi_stream stream);
/* 将主机数据复制到设备缓冲区，NULL 使用默认流。 */
mi_status mi_cuda_copy_from_host(int device, void *dst, const void *host,
                                 size_t bytes, mi_stream stream);
/* 等待指定 CUDA 流完成，NULL 表示等待默认流。 */
mi_status mi_cuda_wait(int device, mi_stream stream);
mi_status mi_cuda_stream_create(int device, mi_stream *out);
void mi_cuda_stream_destroy(int device, mi_stream stream);
/* 兼容旧名称。等待指定流内此前入队的复制。 */
mi_status mi_cuda_wait_event(int device, mi_stream stream);

#ifdef __cplusplus
}
#endif
#endif
