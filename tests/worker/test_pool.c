/*
 * MiniInfer 存储回归层，覆盖 pool。
 * 夹具拥有临时存储和缓冲区。线程与租约在销毁前收尾。
 */
#include "miniinfer/miniinfer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* 在固定预算内验证精确切分和左右两侧合并。 */
static void test_exact_split(void) {
    const size_t unit = 64;
    mi_mempool_config cfg = {.max_cached_bytes = 15 * unit, .max_reserved_bytes = 15 * unit,
                             .alignment = unit, .chunk_bytes = 15 * unit};
    mi_mempool *pool;
    void *a, *b, *c, *d, *out;
    assert(mi_mempool_create(MI_MEMORY_CPU, -1, cfg, &pool) == MI_OK);
    assert(mi_mempool_alloc(pool, 129, 0, &a) == MI_OK);
    assert(mi_mempool_alloc(pool, 257, 0, &b) == MI_OK && (char *)b == (char *)a + 3 * unit);
    assert(mi_mempool_alloc(pool, 65, 0, &c) == MI_OK && (char *)c == (char *)b + 5 * unit);
    assert(mi_mempool_alloc(pool, 257, 0, &d) == MI_OK && (char *)d == (char *)c + 2 * unit);
    assert(mi_mempool_alloc(pool, 1, 0, &out) == MI_ENOMEM);
    assert(mi_mempool_alloc(pool, SIZE_MAX, 0, &out) == MI_EOVERFLOW);

    assert(mi_mempool_release(pool, a) == MI_OK);
    assert(mi_mempool_release(pool, c) == MI_OK);
    assert(mi_mempool_release(pool, b) == MI_OK);
    assert(mi_mempool_alloc(pool, 10 * unit + 1, 0, &out) == MI_ENOMEM);
    assert(mi_mempool_alloc(pool, 10 * unit, 0, &out) == MI_OK && out == a);
    assert(mi_mempool_release(pool, b) == MI_EINVAL);
    assert(mi_mempool_release(pool, out) == MI_OK);
    assert(mi_mempool_release(pool, d) == MI_OK);
    assert(mi_mempool_alloc(pool, 15 * unit, 0, &out) == MI_OK && out == a);
    assert(mi_mempool_release(pool, out) == MI_OK);
    assert(mi_mempool_trim(pool) == MI_OK);

    /* 两次独立分配正好用完预算，其中一次由更严格的对齐要求触发。 */
    assert(mi_mempool_alloc(pool, 1, 2 * unit, &a) == MI_OK && (uintptr_t)a % (2 * unit) == 0);
    assert(mi_mempool_alloc_slab(pool, 15 * unit - 1, 0, &b) == MI_OK);
    assert(mi_mempool_alloc(pool, 1, 0, &out) == MI_ENOMEM);
    assert(mi_mempool_release(pool, a) == MI_OK);
    assert(mi_mempool_release(pool, b) == MI_OK);
    mi_mempool_destroy(pool);
}

/* 桶头小于请求时，位图应定位到下一个二级桶或一级桶。 */
static void test_tlsf_bins(void) {
    const size_t unit = 64;
    const size_t capacity = (64 + 1 + 66 + 1 + 128 + 1) * unit;
    mi_mempool_config cfg = {.max_cached_bytes = capacity, .max_reserved_bytes = capacity,
                             .alignment = unit, .chunk_bytes = capacity};
    mi_mempool *pool;
    void *a, *b, *c, *guards[3], *out;
    assert(mi_mempool_create(MI_MEMORY_CPU, -1, cfg, &pool) == MI_OK);
    assert(mi_mempool_alloc(pool, 64 * unit, 0, &a) == MI_OK);
    assert(mi_mempool_alloc(pool, unit, 0, &guards[0]) == MI_OK);
    assert(mi_mempool_alloc(pool, 66 * unit, 0, &b) == MI_OK);
    assert(mi_mempool_alloc(pool, unit, 0, &guards[1]) == MI_OK);
    assert(mi_mempool_alloc(pool, 128 * unit, 0, &c) == MI_OK);
    assert(mi_mempool_alloc(pool, unit, 0, &guards[2]) == MI_OK);
    assert(mi_mempool_release(pool, a) == MI_OK);
    assert(mi_mempool_release(pool, b) == MI_OK);
    assert(mi_mempool_release(pool, c) == MI_OK);

    assert(mi_mempool_alloc(pool, 65 * unit, 0, &out) == MI_OK && out == b);
    assert(mi_mempool_release(pool, out) == MI_OK);
    assert(mi_mempool_alloc(pool, 67 * unit, 0, &out) == MI_OK && out == c);
    assert(mi_mempool_release(pool, out) == MI_OK);
    for (size_t i = 0; i < 3; ++i)
        assert(mi_mempool_release(pool, guards[i]) == MI_OK);
    assert(mi_mempool_alloc(pool, capacity, 0, &out) == MI_OK && out == a);
    mi_mempool_destroy(pool);
}

static void test_mixed_bucket_and_random(void) {
    mi_mempool *pool;
    const size_t unit=64, cap=136*unit;
    assert(mi_mempool_create(MI_MEMORY_CPU,-1,(mi_mempool_config){.alignment=unit,
        .chunk_bytes=cap,.max_reserved_bytes=cap,.max_cached_bytes=cap},&pool)==MI_OK);
    void *big,*small,*g[2],*out;
    assert(mi_mempool_alloc(pool,67*unit,0,&big)==MI_OK);
    assert(mi_mempool_alloc(pool,unit,0,&g[0])==MI_OK);
    assert(mi_mempool_alloc(pool,66*unit,0,&small)==MI_OK);
    assert(mi_mempool_alloc(pool,unit,0,&g[1])==MI_OK);
    assert(mi_mempool_release(pool,big)==MI_OK);
    assert(mi_mempool_release(pool,small)==MI_OK); /* smaller head in old mapping */
    assert(mi_mempool_alloc(pool,67*unit,0,&out)==MI_OK && out==big);
    assert(mi_mempool_release(pool,out)==MI_OK);
    assert(mi_mempool_release(pool,g[0])==MI_OK);assert(mi_mempool_release(pool,g[1])==MI_OK);
    assert(mi_mempool_alloc(pool,cap,0,&out)==MI_OK);assert(mi_mempool_release(pool,out)==MI_OK);
    mi_mempool_destroy(pool);
    const size_t budget=8*1024*1024;
    assert(mi_mempool_create(MI_MEMORY_CPU,-1,(mi_mempool_config){.alignment=unit,
        .chunk_bytes=budget,.max_reserved_bytes=budget,.max_cached_bytes=budget},&pool)==MI_OK);
    unsigned char *slots[256]={0};size_t sizes[256]={0};uint32_t rng=913;
    for(unsigned k=0;k<300000;k++) {
        rng=rng*1664525u+1013904223u;unsigned i=(rng>>16)%256;
        if(slots[i]) {
            for(size_t j=0;j<sizes[i];j++)assert(slots[i][j]==(unsigned char)i);
            assert(mi_mempool_release(pool,slots[i])==MI_OK);slots[i]=NULL;
        } else {
            rng=rng*1664525u+1013904223u;sizes[i]=1+(rng%16384);
            assert(mi_mempool_alloc(pool,sizes[i],0,(void **)&slots[i])==MI_OK);
            assert((uintptr_t)slots[i]%unit==0);
            memset(slots[i],(int)i,sizes[i]);
        }
    }
    for(unsigned i=0;i<256;i++)if(slots[i])assert(mi_mempool_release(pool,slots[i])==MI_OK);
    assert(mi_mempool_alloc(pool,budget,0,&out)==MI_OK);assert(mi_mempool_release(pool,out)==MI_OK);
    mi_mempool_destroy(pool);
}

/* 验证分桶地址复用、内存预算和锁页路径。 */
int main(void) {
    mi_mempool *pool;
    mi_mempool_config cfg = {.max_cached_bytes = 1024, .max_reserved_bytes = 1024,
                             .alignment = 64, .chunk_bytes = 1024};
    assert(mi_mempool_create(MI_MEMORY_CPU, -1, cfg, &pool) == MI_OK);
    void *a, *b, *c;
    assert(mi_mempool_alloc(pool, 1000, 64, &a) == MI_OK);
    assert(mi_mempool_release(pool, a) == MI_OK);
    assert(mi_mempool_alloc(pool, 500, 64, &b) == MI_OK && a == b);
    assert(mi_mempool_alloc(pool, 512, 64, &c) == MI_OK && (char *)c == (char *)b + 512);
    assert(mi_mempool_alloc(pool, 1, 64, &a) == MI_ENOMEM);
    assert(mi_mempool_release(pool, (char *)c + 64) == MI_EINVAL);
    assert(mi_mempool_release(pool, b) == MI_OK);
    assert(mi_mempool_release(pool, b) == MI_EINVAL);
    assert(mi_mempool_release(pool, c) == MI_OK);
    assert(mi_mempool_alloc(pool, 1000, 64, &a) == MI_OK && a == b);
    assert(mi_mempool_release(pool, a) == MI_OK);
    /* 释放空闲的底层块，归还预留预算。 */
    assert(mi_mempool_trim(pool) == MI_OK);
    mi_mempool_destroy(pool);

    assert(mi_mempool_create(MI_MEMORY_PINNED, -1, (mi_mempool_config){0}, &pool) == MI_OK);
    mi_status st = mi_mempool_alloc_slab(pool, 4096, 8192, &a);
    assert(st == MI_OK || st == MI_ENOMEM);
    if (st == MI_OK) {
        assert((uintptr_t)a % 8192 == 0);
        assert(mi_mempool_release(pool, a) == MI_OK);
    }
    mi_mempool_destroy(pool);

    /* 同一锁页分配的剩余空间仍可切分，不会再次占用页预算。 */
    long page_size = sysconf(_SC_PAGESIZE);
    assert(page_size > 128);
    mi_mempool_config pinned_cfg = {.max_reserved_bytes = (size_t)page_size, .alignment = 64};
    assert(mi_mempool_create(MI_MEMORY_PINNED, -1, pinned_cfg, &pool) == MI_OK);
    st = mi_mempool_alloc(pool, 65, 0, &a);
    assert(st == MI_OK || st == MI_ENOMEM);
    if (st == MI_OK) {
        assert(mi_mempool_alloc(pool, (size_t)page_size - 128, 0, &b) == MI_OK);
        assert((char *)b == (char *)a + 128);
        assert(mi_mempool_release(pool, a) == MI_OK);
        assert(mi_mempool_release(pool, b) == MI_OK);
    }
    mi_mempool_destroy(pool);
    test_mixed_bucket_and_random();
    test_exact_split();
    test_tlsf_bins();
    puts("pool ok");
    return 0;
}
