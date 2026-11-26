/*
 * 存储分配层。池拥有底层分配和主机元数据，块借用底层区间。
 * 锁顺序：后端可进入池锁，池锁不回调调用方。事件退休只访问设备运行时。
 */
#define _GNU_SOURCE
#include "miniinfer/allocator.h"
#include "miniinfer/device_transfer.h"
#include "allocator_internal.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

enum {
    TLSF_SL_BITS = 5,
    TLSF_SL_COUNT = 1 << TLSF_SL_BITS,
    TLSF_FL_COUNT = sizeof(size_t) * CHAR_BIT - TLSF_SL_BITS + 1,
    RADIX_BITS = 6,
    RADIX_SIZE = 1 << RADIX_BITS
};
enum { BLOCK_USED, BLOCK_FREE, BLOCK_PENDING };

typedef struct pool_chunk pool_chunk;

/* 分块元数据位于主机内存，ptr 也可以是 CUDA 设备地址。 */
typedef struct pool_block {
    pool_chunk *chunk;
    struct pool_block *phys_prev, *phys_next; /* 同一底层分配内的相邻块。 */
    struct pool_block *prev, *next;           /* 空闲桶或待回收队列的链接。 */
    unsigned char *ptr;
    size_t bytes;                            /* 池化分块按 unit 对齐。 */
    unsigned state;
    int is_slab;
    mi_event event;                          /* 延迟释放的最后一次 DMA 事件。 */
} pool_block;

/* 首块在合并时总是保留，其余分块使用独立的主机元数据。 */
struct pool_chunk {
    pool_chunk *prev, *next, *idle_prev, *idle_next;
    void *raw;
    size_t bytes; /* 用于归还底层分配和预留预算。 */
    pool_block first;
};

/* 基数树节点按地址位分组索引活跃分配。 */
typedef struct address_node {
    void *slots[RADIX_SIZE];
    unsigned used;
} address_node;

/* 内存池统一管理大小桶、底层分配、地址索引和延迟回收。 */
struct mi_mempool {
    mi_memory_kind kind;
    int device;
    size_t unit, page, chunk_bytes, reserve_limit, cache_limit, reserved, cached;
    pthread_mutex_t mutex;
    pool_block *bins[TLSF_FL_COUNT][TLSF_SL_COUNT];
    uint64_t fl_bitmap;
    uint32_t sl_bitmap[TLSF_FL_COUNT];
    pool_chunk *chunks, *idle;
    pool_block *spare_blocks;
    address_node addresses;
    unsigned unit_shift, address_shift;
    pool_block *pending_head, *pending_tail;
    size_t pending;
    size_t owners;
    int destroy_requested, quarantined, retired, draining;
    struct mi_mempool *retired_next;
};

/* Failed DMA completion retains its owner, rather than freeing live memory.
 * This exceptional list is bounded by the allocations already owned by pools;
 * a poisoned device requires process restart, not speculative memory reuse. */
static pthread_mutex_t retired_mutex = PTHREAD_MUTEX_INITIALIZER;
static mi_mempool *retired_pools;

void mi_mempool_hold(mi_mempool *p) {
    pthread_mutex_lock(&p->mutex);
    ++p->owners;
    pthread_mutex_unlock(&p->mutex);
}

void mi_mempool_drop(mi_mempool *p) {
    pthread_mutex_lock(&p->mutex);
    int destroy = !--p->owners && p->destroy_requested;
    pthread_mutex_unlock(&p->mutex);
    if (destroy)
        mi_mempool_destroy(p);
}

/* 从最高有效位得到以 2 为底的整数对数。 */
static unsigned floor_log2(size_t n) {
    return (unsigned)(sizeof(unsigned long long) * CHAR_BIT - 1 - __builtin_clzll(n));
}

/* GPU addresses are opaque. Radix entries live on the host and never resize. */
static pool_block *address_find(mi_mempool *p, void *ptr) {
    uintptr_t key = (uintptr_t)ptr / p->unit;
    address_node *node = &p->addresses;
    for (unsigned shift = p->address_shift; shift; shift -= RADIX_BITS) {
        node = node->slots[(key >> shift) & (RADIX_SIZE - 1)];
        if (!node)
            return NULL;
    }
    pool_block *block = node->slots[key & (RADIX_SIZE - 1)];
    return block && block->ptr == ptr ? block : NULL;
}

static int address_insert(address_node *node, uintptr_t key, unsigned shift, pool_block *block) {
    unsigned i = (key >> shift) & (RADIX_SIZE - 1);
    if (!shift) {
        node->slots[i] = block;
        ++node->used;
        return 1;
    }
    address_node *child = node->slots[i];
    if (!child) {
        child = calloc(1, sizeof(*child));
        if (!child)
            return 0;
        if (!address_insert(child, key, shift - RADIX_BITS, block)) {
            free(child);
            return 0;
        }
        node->slots[i] = child;
        ++node->used;
        return 1;
    }
    return address_insert(child, key, shift - RADIX_BITS, block);
}

static void address_remove(address_node *node, uintptr_t key, unsigned shift) {
    unsigned i = (key >> shift) & (RADIX_SIZE - 1);
    if (shift) {
        address_node *child = node->slots[i];
        address_remove(child, key, shift - RADIX_BITS);
        if (child->used)
            return;
        free(child);
    }
    node->slots[i] = NULL;
    --node->used;
}

static void address_clear(address_node *node, unsigned shift) {
    if (shift) {
        for (unsigned i = 0; i < RADIX_SIZE; ++i) {
            address_node *child = node->slots[i];
            if (child) {
                address_clear(child, shift - RADIX_BITS);
                free(child);
            }
        }
    }
}

/* Each mlock region owns whole pages. Unmapping one region cannot unlock another. */
static mi_status raw_alloc(mi_mempool *p, pool_chunk *chunk, size_t alignment) {
    size_t bytes = chunk->bytes;
    switch (p->kind) {
    case MI_MEMORY_CPU:
        if (posix_memalign(&chunk->raw, alignment, bytes) != 0)
            return MI_ENOMEM;
        chunk->first.ptr = chunk->raw;
        return MI_OK;
    case MI_MEMORY_PINNED: {
        size_t extra = alignment > p->page ? alignment - p->page : 0;
        if (extra > SIZE_MAX - bytes)
            return MI_EOVERFLOW;
        unsigned char *base = mmap(NULL, bytes + extra, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED)
            return errno == ENOMEM ? MI_ENOMEM : MI_EIO;
        size_t skip = (-(uintptr_t)base) & (alignment - 1);
        unsigned char *ptr = base + skip;
        if (skip)
            munmap(base, skip);
        if (extra > skip)
            munmap(ptr + bytes, extra - skip);
        if (mlock(ptr, bytes) != 0) {
            int error = errno;
            munmap(ptr, bytes);
            return error == ENOMEM || error == EPERM || error == EAGAIN ? MI_ENOMEM : MI_EIO;
        }
        chunk->raw = chunk->first.ptr = ptr;
        return MI_OK;
    }
    case MI_MEMORY_DEVICE:
    case MI_MEMORY_DEVICE_PINNED: {
        mi_status st = p->kind == MI_MEMORY_DEVICE
            ? mi_device_alloc(p->device, bytes, &chunk->raw)
            : mi_device_host_alloc(p->device, bytes, &chunk->raw);
        if (st != MI_OK)
            return st;
        uintptr_t skip = (-(uintptr_t)chunk->raw) & (alignment - 1);
        chunk->first.ptr = (unsigned char *)((uintptr_t)chunk->raw + skip);
        return MI_OK;
    }
    default:
        return MI_EUNSUPPORTED;
    }
}

/* 按内存类型选择对应的系统释放函数。 */
static mi_status raw_free(mi_mempool *p, pool_chunk *chunk) {
    switch (p->kind) {
    case MI_MEMORY_CPU:
        free(chunk->raw);
        return MI_OK;
    case MI_MEMORY_PINNED:
        return munmap(chunk->raw, chunk->bytes) == 0 ? MI_OK : MI_EIO;
    case MI_MEMORY_DEVICE:
        return mi_device_free(p->device, chunk->raw);
    case MI_MEMORY_DEVICE_PINNED:
        return mi_device_host_free(p->device, chunk->raw);
    default:
        return MI_EUNSUPPORTED;
    }
}

/* 小块按 unit 精确分桶，其余每个 2 的幂区间等分为 32 桶。 */
static void bin_indices(const mi_mempool *p, size_t bytes, unsigned *fl, unsigned *sl) {
    size_t units = bytes >> p->unit_shift;
    if (units < TLSF_SL_COUNT) {
        *fl = 0;
        *sl = (unsigned)units;
    } else {
        unsigned shift = floor_log2(units) - TLSF_SL_BITS;
        *fl = shift + 1;
        *sl = (unsigned)(units >> shift) - TLSF_SL_COUNT;
    }
}

/* 把空闲块挂到桶头，并设置两级非空位。 */
static void bin_push(mi_mempool *p, pool_block *b) {
    unsigned fl, sl;
    bin_indices(p, b->bytes, &fl, &sl);
    b->prev = NULL;
    b->next = p->bins[fl][sl];
    if (b->next)
        b->next->prev = b;
    p->bins[fl][sl] = b;
    p->sl_bitmap[fl] |= UINT32_C(1) << sl;
    p->fl_bitmap |= UINT64_C(1) << fl;
    b->state = BLOCK_FREE;
}

/* 摘下块，并清除已空桶对应的位。 */
static void bin_remove(mi_mempool *p, pool_block *b) {
    unsigned fl, sl;
    bin_indices(p, b->bytes, &fl, &sl);
    if (b->prev)
        b->prev->next = b->next;
    else
        p->bins[fl][sl] = b->next;
    if (b->next)
        b->next->prev = b->prev;
    if (!p->bins[fl][sl]) {
        p->sl_bitmap[fl] &= ~(UINT32_C(1) << sl);
        if (!p->sl_bitmap[fl])
            p->fl_bitmap &= ~(UINT64_C(1) << fl);
    }
    b->prev = b->next = NULL;
    b->state = BLOCK_USED;
}

/* Search a size rounded UP to its second-level boundary. Every block in
 * the selected bin is large enough; bitmap selection never scans a free list. */
static pool_block *bin_find(mi_mempool *p, size_t bytes) {
    unsigned fl, sl;
    bin_indices(p, bytes, &fl, &sl);
    uint32_t choices = p->sl_bitmap[fl] & (UINT32_MAX << sl);
    if (!choices) {
        uint64_t larger = p->fl_bitmap & (UINT64_MAX << fl) & ~(UINT64_C(1) << fl);
        if (!larger)
            return NULL;
        fl = (unsigned)__builtin_ctzll(larger);
        choices = p->sl_bitmap[fl];
    }
    return p->bins[fl][__builtin_ctz(choices)];
}

static void idle_remove(mi_mempool *p, pool_chunk *chunk) {
    if (chunk->idle_prev)
        chunk->idle_prev->idle_next = chunk->idle_next;
    else
        p->idle = chunk->idle_next;
    if (chunk->idle_next)
        chunk->idle_next->idle_prev = chunk->idle_prev;
    p->cached -= chunk->bytes;
}

static mi_status drop_chunk(mi_mempool *p, pool_chunk *chunk) {
    mi_status st = raw_free(p, chunk);
    if (st != MI_OK)
        return st;
    if (chunk->prev)
        chunk->prev->next = chunk->next;
    else
        p->chunks = chunk->next;
    if (chunk->next)
        chunk->next->prev = chunk->prev;
    p->reserved -= chunk->bytes;
    return MI_OK;
}

static mi_status drop_idle(mi_mempool *p) {
    pool_chunk *chunk = p->idle;
    mi_status st = drop_chunk(p, chunk);
    if (st == MI_OK) {
        bin_remove(p, &chunk->first);
        idle_remove(p, chunk);
        free(chunk);
    }
    return st;
}

/* 在预留预算内分配新块，必要时先释放空闲大块。 */
static mi_status new_chunk(mi_mempool *p, size_t bytes, size_t alignment, int slab, pool_block **out) {
    size_t raw_bytes = bytes;
    if (p->kind == MI_MEMORY_PINNED) {
        if (bytes > SIZE_MAX - (p->page - 1))
            return MI_EOVERFLOW;
        raw_bytes = (bytes + p->page - 1) & ~(p->page - 1);
    } else if (p->kind == MI_MEMORY_DEVICE_PINNED ||
               (p->kind == MI_MEMORY_DEVICE && alignment > 256)) {
        if (bytes > SIZE_MAX - (alignment - 1))
            return MI_EOVERFLOW;
        raw_bytes += alignment - 1;
    }
    if (raw_bytes > p->reserve_limit - p->reserved + p->cached)
        return MI_ENOMEM;
    while (raw_bytes > p->reserve_limit - p->reserved) {
        mi_status st = drop_idle(p);
        if (st != MI_OK)
            return st;
    }
    pool_chunk *chunk = calloc(1, sizeof(*chunk));
    if (!chunk)
        return MI_ENOMEM;
    chunk->bytes = raw_bytes;
    /* 锁页分配的整页尾部也可以用于池化分块。 */
    size_t usable = !slab && p->kind == MI_MEMORY_PINNED ? raw_bytes : bytes;
    chunk->first = (pool_block){.chunk = chunk, .bytes = usable, .is_slab = slab};
    mi_status st = raw_alloc(p, chunk, alignment);
    if (st != MI_OK) {
        free(chunk);
        return st;
    }
    chunk->next = p->chunks;
    if (p->chunks)
        p->chunks->prev = chunk;
    p->chunks = chunk;
    p->reserved += raw_bytes;
    *out = &chunk->first;
    return MI_OK;
}

/* 按对齐后的请求切一次，剩余分块放回 TLSF。 */
static mi_status split_block(mi_mempool *p, pool_block *b, size_t bytes) {
    if (bytes == b->bytes)
        return MI_OK;

    pool_block *tail = p->spare_blocks;
    if (tail) {
        p->spare_blocks = tail->next;
    } else {
        tail = malloc(sizeof(*tail));
        if (!tail)
            return MI_ENOMEM;
    }
    *tail = (pool_block){.chunk = b->chunk, .phys_prev = b, .phys_next = b->phys_next,
                         .ptr = (unsigned char *)((uintptr_t)b->ptr + bytes),
                         .bytes = b->bytes - bytes};
    if (tail->phys_next)
        tail->phys_next->phys_prev = tail;
    b->phys_next = tail;
    b->bytes = bytes;
    bin_push(p, tail);
    return MI_OK;
}

/* 把已摘下的右邻块并入左块，并缓存右块的元数据。 */
static void merge_next(mi_mempool *p, pool_block *b) {
    pool_block *next = b->phys_next;
    b->bytes += next->bytes;
    b->phys_next = next->phys_next;
    if (b->phys_next)
        b->phys_next->phys_prev = b;
    next->next = p->spare_blocks;
    p->spare_blocks = next;
}

/* 相邻空闲块会立即合并，因此最多检查左右两个块。 */
static void release_binned(mi_mempool *p, pool_block *b) {
    pool_block *prev = b->phys_prev;
    if (prev && prev->state == BLOCK_FREE) {
        bin_remove(p, prev);
        merge_next(p, prev);
        b = prev;
    }
    if (b->phys_next && b->phys_next->state == BLOCK_FREE) {
        bin_remove(p, b->phys_next);
        merge_next(p, b);
    }
    if (!b->phys_prev && !b->phys_next) {
        pool_chunk *chunk = b->chunk;
        if ((p->cached >= p->cache_limit || chunk->bytes > p->cache_limit - p->cached) &&
            drop_chunk(p, chunk) == MI_OK) {
            free(chunk);
            return;
        }
        chunk->idle_prev = NULL;
        chunk->idle_next = p->idle;
        if (p->idle)
            p->idle->idle_prev = chunk;
        p->idle = chunk;
        p->cached += chunk->bytes;
    }
    bin_push(p, b);
}

/* 注销地址后归还独立区或合并分桶块。 */
static mi_status release_block(mi_mempool *p, pool_block *b) {
    pool_chunk *chunk = b->chunk;
    if (b->is_slab) {
        mi_status st = drop_chunk(p, chunk);
        if (st != MI_OK)
            return st;
    }
    address_remove(&p->addresses, (uintptr_t)b->ptr / p->unit, p->address_shift);
    if (b->is_slab)
        free(chunk);
    else
        release_binned(p, b);
    return MI_OK;
}

/* Bounded rotating polls keep an unrelated stream from blocking reuse. */
static mi_status collect_locked(mi_mempool *p, size_t limit, int synchronized) {
    if (p->draining)
        return MI_OK;
    if (limit > p->pending)
        limit = p->pending;
    while (limit--) {
        pool_block *b = p->pending_head;
        mi_status result = synchronized ? MI_OK : mi_device_event_ready(p->device, b->event);
        if (result != MI_OK && result != MI_EBUSY)
            return result;

        pool_block *next = b->next;
        if (result == MI_OK) {
            mi_event event = b->event;
            mi_status st = release_block(p, b);
            if (st != MI_OK)
                return st;
            (void)mi_device_event_destroy(p->device, event);
            --p->pending;
            p->pending_head = next;
            if (!next)
                p->pending_tail = NULL;
        } else if (next) {
            p->pending_head = next;
            b->next = NULL;
            p->pending_tail->next = b;
            p->pending_tail = b;
        }
    }
    return MI_OK;
}

mi_status mi_mempool_create(mi_memory_kind kind, int device, mi_mempool_config config,
                            mi_mempool **out) {
    size_t unit = config.alignment ? config.alignment : 64;
    if (!out || (unsigned)kind > MI_MEMORY_DEVICE_PINNED || unit < sizeof(void *) ||
        (unit & (unit - 1)) || (kind >= MI_MEMORY_DEVICE && device < 0))
        return MI_EINVAL;
    *out = NULL;
    mi_mempool *p = calloc(1, sizeof(*p));
    if (!p)
        return MI_ENOMEM;
    p->kind = kind;
    p->device = kind >= MI_MEMORY_DEVICE ? device : -1;
    p->unit = kind == MI_MEMORY_DEVICE && unit < 256 ? 256 : unit;
    long page = kind == MI_MEMORY_PINNED ? sysconf(_SC_PAGESIZE) : 1;
    if (page < 1 || pthread_mutex_init(&p->mutex, NULL) != 0) {
        free(p);
        return MI_EINTERNAL;
    }
    p->page = (size_t)page;
    p->reserve_limit = config.max_reserved_bytes ? config.max_reserved_bytes : SIZE_MAX;
    p->cache_limit = config.max_cached_bytes;
    p->chunk_bytes = config.chunk_bytes ? config.chunk_bytes : 1024 * 1024;
    if (p->chunk_bytes > p->cache_limit)
        p->chunk_bytes = p->cache_limit;
    p->unit_shift = floor_log2(p->unit);
    p->address_shift = (unsigned)(sizeof(uintptr_t) * CHAR_BIT - 1 - p->unit_shift) /
        RADIX_BITS * RADIX_BITS;
    mi_status st = mi_device_init(p->device);
    if (st != MI_OK) {
        pthread_mutex_destroy(&p->mutex);
        free(p);
        return st;
    }
    *out = p;
    return MI_OK;
}

mi_memory_kind mi_mempool_kind(const mi_mempool *p) {
    return p->kind;
}

int mi_mempool_device(const mi_mempool *p) {
    return p->device;
}

static mi_status allocate(mi_mempool *p, size_t bytes, size_t alignment, int slab, void **out) {
    if (!p || !out || !bytes || (alignment & (alignment - 1)))
        return MI_EINVAL;
    if (alignment < p->unit)
        alignment = p->unit;
    slab |= alignment > p->unit;

    size_t exact_bytes = bytes;
    size_t unit_mask = p->unit - 1;
    if (!slab) {
        if (bytes > SIZE_MAX - unit_mask)
            return MI_EOVERFLOW;
        bytes = (bytes + unit_mask) & ~unit_mask;
        exact_bytes = bytes;
        size_t units = bytes >> p->unit_shift;
        if (units >= TLSF_SL_COUNT) {
            unsigned shift = floor_log2(units) - TLSF_SL_BITS;
            size_t round = ((size_t)1 << shift) - 1;
            if (units > (SIZE_MAX >> p->unit_shift) - round)
                return MI_EOVERFLOW;
            bytes = ((units + round) & ~round) << p->unit_shift;
        }
    }
    pthread_mutex_lock(&p->mutex);
    pool_block *b = NULL;
    mi_status st = p->quarantined || p->destroy_requested ? MI_EIO : collect_locked(p, 4, 0);
    if (st != MI_OK)
        goto done;
    if (slab) {
        st = new_chunk(p, bytes, alignment, 1, &b);
    } else {
        unsigned fl, sl;
        bin_indices(p, exact_bytes, &fl, &sl);
        pool_block *exact = p->bins[fl][sl];
        if (exact && exact->bytes == exact_bytes) {
            b = exact; /* Preserve exact fits, including a fully coalesced pool. */
            bytes = exact_bytes;
        } else {
            b = bin_find(p, bytes);
        }
        if (b) {
            if (!b->phys_prev && !b->phys_next)
                idle_remove(p, b->chunk);
            bin_remove(p, b);
        } else {
            size_t available = p->reserve_limit - p->reserved;
            size_t preferred = p->chunk_bytes < available ? p->chunk_bytes : available;
            preferred &= ~unit_mask;
            size_t chunk_bytes = preferred > bytes ? preferred : bytes;
            st = new_chunk(p, chunk_bytes, alignment, 0, &b);
            if (st == MI_ENOMEM && chunk_bytes > bytes)
                st = new_chunk(p, bytes, alignment, 0, &b);
        }
    }
    if (st != MI_OK)
        goto done;
    if (!slab)
        st = split_block(p, b, bytes);

    /* 返回地址前登记活跃分配，登记失败时回收该块。 */
    if (st == MI_OK && !address_insert(&p->addresses, (uintptr_t)b->ptr / p->unit, p->address_shift, b))
        st = MI_ENOMEM;
    if (st == MI_OK)
        *out = b->ptr;
    else if (slab) {
        if (drop_chunk(p, b->chunk) == MI_OK)
            free(b->chunk);
    } else
        release_binned(p, b);

done:
    pthread_mutex_unlock(&p->mutex);
    return st;
}

mi_status mi_mempool_alloc(mi_mempool *p, size_t bytes, size_t alignment, void **out) {
    return allocate(p, bytes, alignment, 0, out);
}

mi_status mi_mempool_alloc_slab(mi_mempool *p, size_t bytes, size_t alignment, void **out) {
    return allocate(p, bytes, alignment, 1, out);
}

mi_status mi_mempool_release(mi_mempool *p, void *ptr) {
    if (!p)
        return MI_EINVAL;
    if (!ptr)
        return MI_OK;
    pthread_mutex_lock(&p->mutex);
    pool_block *b = address_find(p, ptr);
    mi_status st = p->quarantined ? MI_EIO
        : b && b->state == BLOCK_USED ? release_block(p, b) : MI_EINVAL;
    pthread_mutex_unlock(&p->mutex);
    return st;
}

mi_status mi_mempool_release_async(mi_mempool *p, void *ptr, mi_stream stream) {
    if (!p || !ptr || p->device < 0)
        return MI_EINVAL;
    pthread_mutex_lock(&p->mutex);
    pool_block *b = address_find(p, ptr);
    if (!b || b->state != BLOCK_USED) {
        pthread_mutex_unlock(&p->mutex);
        return MI_EINVAL;
    }
    /* Pin metadata before dropping the lock for stream/event operations. */
    b->state = BLOCK_PENDING;
    pthread_mutex_unlock(&p->mutex);
    mi_event event = NULL;
    mi_status st = mi_device_event_create(p->device, &event);
    if (st == MI_OK)
        st = mi_device_event_record(p->device, event, stream);
    if (st == MI_OK) {
        pthread_mutex_lock(&p->mutex);
        b->event = event;
        /* CUDA 事件完成前，块保持待回收状态。 */
        b->state = BLOCK_PENDING;
        b->next = NULL;
        if (p->pending_tail)
            p->pending_tail->next = b;
        else
            p->pending_head = b;
        p->pending_tail = b;
        ++p->pending;
    } else {
        (void)mi_device_event_destroy(p->device, event);
        mi_status waited = mi_device_stream_wait(p->device, stream);
        pthread_mutex_lock(&p->mutex);
        if (waited == MI_OK)
            st = release_block(p, b);
        else {
            p->quarantined = 1;
            st = MI_EIO;
        }
    }
    pthread_mutex_unlock(&p->mutex);
    return st;
}

mi_status mi_mempool_collect(mi_mempool *p) {
    if (!p)
        return MI_EINVAL;
    pthread_mutex_lock(&p->mutex);
    mi_status st = p->quarantined ? MI_EIO : collect_locked(p, p->pending, 0);
    pthread_mutex_unlock(&p->mutex);
    return st;
}

mi_status mi_mempool_trim(mi_mempool *p) {
    if (!p)
        return MI_EINVAL;
    pthread_mutex_lock(&p->mutex);
    if (p->draining || p->quarantined) {
        mi_status st = p->quarantined ? MI_EIO : MI_EBUSY;
        pthread_mutex_unlock(&p->mutex);
        return st;
    }
    p->draining = 1;
    size_t pending = p->pending;
    pool_block *b = p->pending_head;
    pthread_mutex_unlock(&p->mutex);
    mi_status st = MI_OK;
    for (size_t i = 0; i < pending && st == MI_OK; ++i) {
        st = mi_device_event_wait(p->device, b->event);
        if (i + 1 < pending)
            b = b->next;
    }
    pthread_mutex_lock(&p->mutex);
    p->draining = 0;
    if (st == MI_OK && !p->quarantined)
        st = collect_locked(p, pending, 1);
    else if (p->quarantined)
        st = MI_EIO;
    while (st == MI_OK && p->idle)
        st = drop_idle(p);
    pthread_mutex_unlock(&p->mutex);
    return st;
}

void mi_mempool_destroy(mi_mempool *p) {
    if (!p)
        return;
    pthread_mutex_lock(&p->mutex);
    p->destroy_requested = 1;
    if (p->owners || p->retired) {
        pthread_mutex_unlock(&p->mutex);
        return;
    }
    /* Only pending events require a fence. Never use a device-wide sync to
     * hide an uncertain stream, or free after a failed completion check. */
    pool_block *pending = p->pending_head;
    pthread_mutex_unlock(&p->mutex);
    int failed = p->quarantined;
    for (pool_block *b = pending; b && !failed; b = b->next)
        failed = mi_device_event_wait(p->device, b->event) != MI_OK;
    pthread_mutex_lock(&p->mutex);
    p->quarantined |= failed;
    if (p->quarantined) {
        p->retired = 1;
        pthread_mutex_unlock(&p->mutex);
        pthread_mutex_lock(&retired_mutex);
        p->retired_next = retired_pools;
        retired_pools = p;
        pthread_mutex_unlock(&retired_mutex);
        return;
    }
    pthread_mutex_unlock(&p->mutex);
    while (p->chunks) {
        pool_chunk *chunk = p->chunks;
        p->chunks = chunk->next;
        (void)raw_free(p, chunk);
        pool_block *b = &chunk->first;
        while (b) {
            pool_block *next = b->phys_next;
            if (b->state == BLOCK_PENDING)
                (void)mi_device_event_destroy(p->device, b->event);
            if (b != &chunk->first)
                free(b);
            b = next;
        }
        free(chunk);
    }
    while (p->spare_blocks) {
        pool_block *b = p->spare_blocks;
        p->spare_blocks = b->next;
        free(b);
    }
    address_clear(&p->addresses, p->address_shift);
    pthread_mutex_destroy(&p->mutex);
    (void)mi_device_cleanup(p->device);
    free(p);
}
