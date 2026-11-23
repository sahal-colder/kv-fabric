/*
 * 模型文件层。验证布局并创建只读映射。
 * 映射对象拥有 mmap 区间，张量视图借用地址。无内部锁。
 */
#ifndef MINIINFER_MODEL_MAPPING_H
#define MINIINFER_MODEL_MAPPING_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 只读映射权重文件。文件至少需有 min_bytes 字节。 */
mi_status mi_model_map(const char *path, size_t min_bytes, void **data, size_t *bytes);
/* 解除 mi_model_map 返回的映射。 */
void mi_model_unmap(void *data, size_t bytes);

#ifdef __cplusplus
}
#endif
#endif
