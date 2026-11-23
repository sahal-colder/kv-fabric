/*
 * 设备内存层。管理图捕获期间的工作区和对齐子分配。
 * 分配器保留内存池中的工作区，图使用者借用。图状态锁不进入 Storage。
 */
#include "miniinfer/graph_allocator.h"
#include <pthread.h>
#include <stdlib.h>

enum { GRAPH_ALIGNMENT = 256 };

typedef struct graph_record {
    mi_graph_id id;
    size_t bytes;
} graph_record;

/* 持有固定工作区、捕获状态和按图 ID 索引的用量。 */
struct mi_graph_allocator {
    mi_mempool *pool;
    unsigned char *workspace;
    size_t bytes, offset, max_allocations, allocations, live;
    uint64_t epoch, *active;
    graph_record *graphs;
    size_t graph_mask, graph_count;
    mi_graph_id capture_id;
    mi_stream stream;
    pthread_t owner;
    pthread_mutex_t mutex;
};

static size_t graph_bucket(const mi_graph_allocator *g, mi_graph_id id) {
    id ^= id >> 33;
    id *= UINT64_C(0xff51afd7ed558ccd);
    id ^= id >> 33;
    return id & g->graph_mask;
}

/* 线性探测，返回图记录或首个空桶。 */
static size_t graph_slot(const mi_graph_allocator *g, mi_graph_id id) {
    size_t i = graph_bucket(g, id);
    while (g->graphs[i].id && g->graphs[i].id != id)
        i = (i + 1) & g->graph_mask;
    return i;
}

mi_status mi_graph_allocator_create(mi_mempool *pool, size_t bytes, size_t max_allocations,
                                    mi_graph_allocator **out) {
    if (!pool || !out || bytes < GRAPH_ALIGNMENT || !max_allocations ||
        max_allocations > SIZE_MAX / sizeof(graph_record) / 4)
        return MI_EINVAL;
    bytes &= ~(size_t)(GRAPH_ALIGNMENT - 1);
    size_t graph_capacity = 2;
    while (graph_capacity / 2 < max_allocations)
        graph_capacity *= 2;

    mi_graph_allocator *g = calloc(1, sizeof(*g));
    if (!g)
        return MI_ENOMEM;
    g->graphs = calloc(graph_capacity, sizeof(*g->graphs));
    g->active = calloc(bytes / GRAPH_ALIGNMENT, sizeof(*g->active));
    mi_status st = MI_ENOMEM;
    if (!g->graphs || !g->active)
        goto failed;
    st = MI_EINTERNAL;
    if (pthread_mutex_init(&g->mutex, NULL) != 0)
        goto failed;
    void *workspace;
    st = mi_mempool_alloc_slab(pool, bytes, GRAPH_ALIGNMENT, &workspace);
    if (st != MI_OK) {
        pthread_mutex_destroy(&g->mutex);
        goto failed;
    }
    g->pool = pool;
    g->workspace = workspace;
    g->bytes = bytes;
    g->max_allocations = max_allocations;
    g->graph_mask = graph_capacity - 1;
    *out = g;
    return MI_OK;

failed:
    free(g->active);
    free(g->graphs);
    free(g);
    return st;
}

mi_status mi_graph_begin_capture(mi_graph_allocator *g, mi_graph_id id, mi_stream stream) {
    if (!g || !id || !stream)
        return MI_EINVAL;
    pthread_mutex_lock(&g->mutex);
    mi_status st = MI_OK;
    if (!g->workspace)
        st = MI_ESTATE;
    else if (g->capture_id || g->graphs[graph_slot(g, id)].id)
        st = MI_EBUSY;
    else if (g->graph_count == g->max_allocations)
        st = MI_ENOMEM;
    else if (g->epoch == UINT64_MAX)
        st = MI_EOVERFLOW;
    else {
        ++g->epoch;
        g->capture_id = id;
        g->stream = stream;
        g->owner = pthread_self();
        /* 每次捕获从同一地址开始分配。 */
        g->offset = g->allocations = g->live = 0;
    }
    pthread_mutex_unlock(&g->mutex);
    return st;
}

/* 只允许捕获线程通过绑定的流操作工作区。 */
static mi_status check_owner(const mi_graph_allocator *g, mi_stream stream) {
    if (!g->capture_id)
        return MI_ESTATE;
    return g->stream == stream && pthread_equal(g->owner, pthread_self()) ? MI_OK : MI_EBUSY;
}

/* A capture epoch invalidates every old slice when the bump offset resets. */
mi_status mi_graph_alloc(mi_graph_allocator *g, size_t bytes, mi_stream stream, void **out) {
    if (!g || !out || !bytes)
        return MI_EINVAL;
    pthread_mutex_lock(&g->mutex);
    mi_status st = check_owner(g, stream);
    if (st == MI_OK) {
        if (bytes > g->bytes - g->offset || g->allocations == g->max_allocations) {
            st = MI_ENOMEM;
        } else {
            *out = g->workspace + g->offset;
            g->active[g->offset / GRAPH_ALIGNMENT] = g->epoch;
            g->offset += (bytes + GRAPH_ALIGNMENT - 1) & ~(size_t)(GRAPH_ALIGNMENT - 1);
            ++g->allocations;
            ++g->live;
        }
    }
    pthread_mutex_unlock(&g->mutex);
    return st;
}

mi_status mi_graph_release(mi_graph_allocator *g, void *ptr, mi_stream stream) {
    if (!g || !ptr)
        return MI_EINVAL;
    pthread_mutex_lock(&g->mutex);
    mi_status st = check_owner(g, stream);
    if (st == MI_OK) {
        uintptr_t offset = (uintptr_t)ptr - (uintptr_t)g->workspace;
        if (offset >= g->offset || offset % GRAPH_ALIGNMENT ||
            g->active[offset / GRAPH_ALIGNMENT] != g->epoch) {
            st = MI_EINVAL;
        } else {
            g->active[offset / GRAPH_ALIGNMENT] = 0;
            --g->live;
        }
    }
    pthread_mutex_unlock(&g->mutex);
    return st;
}

/* 正常结束时登记图的用量，取消时直接清理捕获状态。 */
static mi_status finish_capture(mi_graph_allocator *g, mi_graph_id id, int aborting) {
    pthread_mutex_lock(&g->mutex);
    mi_status st = check_owner(g, g->stream);
    if (st == MI_OK) {
        if (g->capture_id != id)
            st = MI_EINVAL;
        else if (g->live && !aborting)
            st = MI_EBUSY;
        else {
            if (!aborting) {
                g->graphs[graph_slot(g, id)] = (graph_record){id, g->offset};
                ++g->graph_count;
            }
            g->capture_id = 0;
            g->stream = NULL;
        }
    }
    pthread_mutex_unlock(&g->mutex);
    return st;
}

mi_status mi_graph_end_capture(mi_graph_allocator *g, mi_graph_id id) {
    return g ? finish_capture(g, id, 0) : MI_EINVAL;
}

mi_status mi_graph_abort_capture(mi_graph_allocator *g, mi_graph_id id) {
    return g ? finish_capture(g, id, 1) : MI_EINVAL;
}

mi_status mi_graph_workspace_used(mi_graph_allocator *g, mi_graph_id id, size_t *out) {
    if (!g || !id || !out)
        return MI_EINVAL;
    pthread_mutex_lock(&g->mutex);
    graph_record r = g->graphs[graph_slot(g, id)];
    if (r.id)
        *out = r.bytes;
    pthread_mutex_unlock(&g->mutex);
    return r.id ? MI_OK : MI_ENOENT;
}

/* Close the gap in the probe chain after removing a graph. */
mi_status mi_graph_forget(mi_graph_allocator *g, mi_graph_id id) {
    if (!g || !id)
        return MI_EINVAL;
    pthread_mutex_lock(&g->mutex);
    size_t gap = graph_slot(g, id);
    if (!g->graphs[gap].id) {
        pthread_mutex_unlock(&g->mutex);
        return MI_ENOENT;
    }
    size_t mask = g->graph_mask;
    for (size_t i = (gap + 1) & mask; g->graphs[i].id; i = (i + 1) & mask) {
        size_t home = graph_bucket(g, g->graphs[i].id);
        if (((gap - home) & mask) < ((i - home) & mask)) {
            g->graphs[gap] = g->graphs[i];
            gap = i;
        }
    }
    g->graphs[gap] = (graph_record){0};
    --g->graph_count;
    pthread_mutex_unlock(&g->mutex);
    return MI_OK;
}

mi_status mi_graph_release_workspace(mi_graph_allocator *g) {
    if (!g)
        return MI_EINVAL;
    pthread_mutex_lock(&g->mutex);
    mi_status st = g->capture_id || g->graph_count ? MI_EBUSY
        : mi_mempool_release(g->pool, g->workspace);
    if (st == MI_OK)
        g->workspace = NULL;
    pthread_mutex_unlock(&g->mutex);
    return st;
}

void mi_graph_allocator_destroy(mi_graph_allocator *g) {
    if (!g)
        return;
    (void)mi_mempool_release(g->pool, g->workspace);
    pthread_mutex_destroy(&g->mutex);
    free(g->active);
    free(g->graphs);
    free(g);
}
