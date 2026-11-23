/*
 * MiniInfer 存储回归层，覆盖 paged kv。
 * 夹具拥有临时存储和缓冲区。线程与租约在销毁前收尾。
 */
#include "miniinfer/miniinfer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* 验证完整页发布后可以匹配和共享，封存页只读。 */
int main(void) {
    mi_mempool *pool = NULL;
    mi_storage_backend *cpu = NULL;
    mi_tiered_storage *s = NULL;
    mi_storage_config sc;
    mi_storage_backends b;
    mi_paged_kv *k = NULL;
    mi_kv_sequence *q = NULL, *hit = NULL;
    mi_page_lease l;
    int32_t tokens[2] = {42, 43};
    char payload[4096];
    size_t matched;
    memset(payload, 0x7b, sizeof(payload));
    assert(mi_mempool_create(MI_MEMORY_CPU, -1, (mi_mempool_config){0}, &pool) == MI_OK);
    assert(mi_memory_backend_create(MI_LOCATION_CPU, 4096, pool, 8, &cpu) == MI_OK);
    mi_storage_config_default(&sc);
    sc.block_bytes = 4096;
    sc.cpu_bytes = 4096 * 8;
    b = (mi_storage_backends){.cpu = cpu, .ssd = NULL};
    assert(mi_tiered_storage_create(sc, b, &s) == MI_OK);
    assert(mi_paged_kv_create(s, 2, 2, 4096, "model-a", &k) == MI_OK);
    assert(mi_kv_sequence_create(k, &q) == MI_OK);
    assert(mi_kv_sequence_append_page(q, MI_LOCATION_CPU) == MI_OK);
    assert(mi_paged_kv_acquire(q, 0, 0, MI_ACCESS_WRITE, NULL, &l) == MI_OK);
    memcpy(l.lease.data, payload, sizeof(payload));
    assert(mi_page_lease_finish(&l) == MI_OK);
    assert(mi_paged_kv_acquire(q, 0, 1, MI_ACCESS_WRITE, NULL, &l) == MI_OK);
    memcpy(l.lease.data, payload, sizeof(payload));
    assert(mi_page_lease_finish(&l) == MI_OK);
    assert(mi_kv_sequence_publish(q, tokens, 2, 1) == MI_OK);
    assert(mi_paged_kv_match(k, tokens, 2, &hit, &matched) == MI_OK);
    assert(matched == 2 && mi_kv_sequence_page_count(hit) == 1);
    assert(mi_paged_kv_acquire(hit, 0, 0, MI_ACCESS_READ, NULL, &l) == MI_OK);
    assert(memcmp(l.lease.data, payload, sizeof(payload)) == 0);
    assert(mi_page_lease_finish(&l) == MI_OK);
    mi_kv_sequence_destroy(hit);
    mi_kv_sequence_destroy(q);
    assert(mi_paged_kv_evict_prefixes(k) == MI_OK);
    mi_paged_kv_destroy(k);
    mi_tiered_storage_destroy(s);
    mi_backend_destroy(cpu);
    mi_mempool_destroy(pool);
    puts("paged kv ok");
    return 0;
}
