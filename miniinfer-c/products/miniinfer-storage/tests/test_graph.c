/*
 * MiniInfer 存储回归层，覆盖 graph。
 * 夹具拥有临时存储和缓冲区。线程与租约在销毁前收尾。
 */
#include "miniinfer/miniinfer.h"
#include <assert.h>
#include <stdio.h>
/* 验证捕获用量、切片归还和下一次捕获的地址复用。 */
int main(void) {
    mi_mempool *pool = NULL;
    mi_graph_allocator *g = NULL;
    void *p, *q;
    size_t used;
    int stream_token = 1;
    assert(mi_mempool_create(MI_MEMORY_CPU, -1, (mi_mempool_config){0}, &pool) == MI_OK);
    assert(mi_graph_allocator_create(pool, 4096, 16, &g) == MI_OK);
    assert(mi_graph_begin_capture(g, 7, &stream_token) == MI_OK);
    assert(mi_graph_alloc(g, 100, &stream_token, &p) == MI_OK);
    assert(mi_graph_alloc(g, 300, &stream_token, &q) == MI_OK);
    assert(mi_graph_release(g, p, &stream_token) == MI_OK);
    assert(mi_graph_release(g, q, &stream_token) == MI_OK);
    assert(mi_graph_end_capture(g, 7) == MI_OK);
    assert(mi_graph_workspace_used(g, 7, &used) == MI_OK && used == 768);
    assert(mi_graph_release_workspace(g) == MI_EBUSY);
    void *same;
    assert(mi_graph_begin_capture(g, 8, &stream_token) == MI_OK);
    assert(mi_graph_alloc(g, 100, &stream_token, &same) == MI_OK && same == p);
    assert(mi_graph_end_capture(g, 8) == MI_EBUSY);
    assert(mi_graph_abort_capture(g, 8) == MI_OK);
    assert(mi_graph_forget(g, 7) == MI_OK);
    assert(mi_graph_release_workspace(g) == MI_OK);
    mi_graph_allocator_destroy(g);
    mi_mempool_destroy(pool);
    puts("graph ok");
    return 0;
}
