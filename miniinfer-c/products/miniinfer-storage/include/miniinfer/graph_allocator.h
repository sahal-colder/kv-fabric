/*
 * 设备内存层。管理图捕获期间的工作区和对齐子分配。
 * 分配器保留内存池中的工作区，图使用者借用。图状态锁不进入 Storage。
 */
#ifndef MINIINFER_GRAPH_ALLOCATOR_H
#define MINIINFER_GRAPH_ALLOCATOR_H
#include "allocator.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 由调用方指定的图标识，0 保留。 */
typedef uint64_t mi_graph_id;
typedef struct mi_graph_allocator mi_graph_allocator;

/* 图共享同一工作区。重放须串行，归还工作区前须移除所有图的登记。 */

/* 从 pool 借用固定地址工作区。max_allocations 限制每次捕获的分配次数及登记的图数。 */
mi_status mi_graph_allocator_create(mi_mempool *pool,
                                    size_t workspace_bytes,
                                    size_t max_allocations,
                                    mi_graph_allocator **out);

/* 绑定当前线程与流，从工作区起点开始分配。 */
mi_status mi_graph_begin_capture(mi_graph_allocator *ga, mi_graph_id id, mi_stream stream);
/* 从工作区分配对齐切片，地址写入 out。 */
mi_status mi_graph_alloc(mi_graph_allocator *ga, size_t bytes, mi_stream stream, void **out);
/* 结束切片使用。本轮捕获的分配偏移继续向前。 */
mi_status mi_graph_release(mi_graph_allocator *ga, void *ptr, mi_stream stream);

/* 结束捕获并登记用量。所有切片须先归还。 */
mi_status mi_graph_end_capture(mi_graph_allocator *ga, mi_graph_id id);
/* 放弃当前捕获并重置其状态。 */
mi_status mi_graph_abort_capture(mi_graph_allocator *ga, mi_graph_id id);
/* 返回已登记图所用的工作区字节数。 */
mi_status mi_graph_workspace_used(mi_graph_allocator *ga, mi_graph_id id, size_t *out);

/* 删除图的工作区登记。调用方须先结束该图的使用。 */
mi_status mi_graph_forget(mi_graph_allocator *ga, mi_graph_id id);
/* 归还工作区。仍有捕获或登记的图时返回 MI_EBUSY。 */
mi_status mi_graph_release_workspace(mi_graph_allocator *ga);
/* 释放分配器。销毁前须结束工作区使用。 */
void mi_graph_allocator_destroy(mi_graph_allocator *ga);

#ifdef __cplusplus
}
#endif
#endif
