/*
 * 存储 SDK 聚合头。选择公开 C 接口供调用者包含。
 * 本头不分配内存，不引入额外锁序。
 */
#ifndef MINIINFER_MINIINFER_H
#define MINIINFER_MINIINFER_H

/* MiniInfer Storage 公共 API 入口。 */
#include "common.h"
#include "model_mapping.h"
#include "file_io.h"
#include "cuda_adapter.h"
#include "allocator.h"
#include "graph_allocator.h"
#include "storage_backend.h"
#include "tiered_storage.h"
#include "kv_cache.h"
#include "paged_kv.h"
#endif
