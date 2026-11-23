/*
 * MiniInfer 存储回归层，覆盖 kv cache。
 * 夹具拥有临时存储和缓冲区。线程与租约在销毁前收尾。
 */
#include "miniinfer/miniinfer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* 用有限的 CPU 容量触发 KV 卸载、读回和后台预取。 */
int main(void) {
    mi_mempool *pool = NULL;
    mi_storage_backend *cpu = NULL, *ssd = NULL;
    mi_tiered_storage *s = NULL;
    mi_storage_config sc;
    mi_storage_backends b;
    mi_kv_cache *c = NULL;
    mi_kv_cache_config kc = {.logical_slots = 4, .payload_bytes = 4096, .prefetch_queue_depth = 4, .enable_prefetch_worker = 1};
    char a[4096], d[4096], e[4096];
    size_t n;
    memset(a, 0x11, sizeof(a));
    memset(d, 0x22, sizeof(d));
    assert(mi_mempool_create(MI_MEMORY_CPU, -1, (mi_mempool_config){0}, &pool) == MI_OK);
    assert(mi_memory_backend_create(MI_LOCATION_CPU, 4096, pool, 4, &cpu) == MI_OK);
    assert(mi_ssd_backend_create(NULL, 4096, (4096 + 64) * 8, MI_SSD_VERIFY_ON_READ, &ssd) == MI_OK);
    mi_storage_config_default(&sc);
    sc.block_bytes = 4096;
    sc.cpu_bytes = 4096;
    sc.ssd_bytes = (4096 + 64) * 8;
    b = (mi_storage_backends){.cpu = cpu, .ssd = ssd};
    assert(mi_tiered_storage_create(sc, b, &s) == MI_OK);
    assert(mi_kv_cache_create(s, kc, &c) == MI_OK);
    assert(mi_kv_cache_write(c, 0, a, sizeof(a)) == MI_OK);
    assert(mi_kv_cache_write(c, 1, d, sizeof(d)) == MI_OK);
    assert(mi_kv_cache_read(c, 0, e, sizeof(e), &n) == MI_OK && n == sizeof(e));
    assert(memcmp(a, e, sizeof(a)) == 0);

    assert(mi_kv_cache_prefetch_async(c, 1) == MI_OK);
    assert(mi_kv_cache_wait_prefetch(c, 1) == MI_OK);
    assert(mi_kv_cache_read(c, 1, e, sizeof(e), &n) == MI_OK && n == sizeof(e));
    assert(memcmp(d, e, sizeof(d)) == 0);
    mi_kv_cache_destroy(c);
    mi_tiered_storage_destroy(s);
    mi_backend_destroy(ssd);
    mi_backend_destroy(cpu);
    mi_mempool_destroy(pool);
    puts("kv cache ok");
    return 0;
}
