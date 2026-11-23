/*
 * 单机推理的可选 CUDA 内核接口。Tensor 所有权归推理引擎。
 * 同一请求的设备操作沿其 stream 排序。此头不引入存储锁。
 */
#ifndef MINIINFER_NANO_CUDA_H
#define MINIINFER_NANO_CUDA_H

#include "nv.h"

#ifdef __cplusplus
extern "C" {
#endif
/* 更新 token 与页表后重放 CUDA Graph，返回主机 logits。 */
float *nv_cuda_forward(NvCache *cache, const int *table, int token, int pos);
/* 初始化指定设备的缓冲区、流和图。 */
void nv_cuda_init(NvEngine *engine, int device);
/* 在主机与设备之间复制完整 KV 页。 */
void nv_cuda_copy_block(NvCache *cache, int block, void *host, size_t half, int to_device);
#ifdef NANO_WITH_REMOTE_KV
/* Returns success only after the stream event completes both K and V copies. */
int nv_cuda_copy_remote_block(NvCache *cache, int block, void *host, size_t half, int to_device);
#endif
/* 释放 CUDA 图、流和缓冲区。 */
void nv_cuda_destroy(struct NvCuda *cuda);
#ifdef __cplusplus
}
#endif
#endif
