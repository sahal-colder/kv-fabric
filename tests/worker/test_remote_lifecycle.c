/*
 * MiniInfer 存储回归层，覆盖 remote lifecycle。
 * 夹具拥有临时存储和缓冲区。线程与租约在销毁前收尾。
 */
#include "miniinfer/paged_kv.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { BYTES = 4096, SLOTS = 4 };
static void write_layer(mi_kv_sequence *s, size_t page, size_t layer) {
    mi_page_lease l;
    assert(mi_paged_kv_acquire(s, page, layer, MI_ACCESS_WRITE, NULL, &l) == MI_OK);
    memset(l.lease.data, (int)(page + layer + 1), BYTES);
    assert(mi_page_lease_finish(&l) == MI_OK);
}

int main(void) {
    mi_mempool *pool;
    mi_storage_backend *cpu, *ssd;
    mi_tiered_storage *storage;
    mi_paged_kv *cache;
    mi_kv_sequence *s, *found;
    const int32_t tokens[] = {11, 12, 13, 14};
    assert(mi_mempool_create(MI_MEMORY_CPU, -1, (mi_mempool_config){0}, &pool) == MI_OK);
    assert(mi_memory_backend_create(MI_LOCATION_CPU, BYTES, pool, SLOTS, &cpu) == MI_OK);
    assert(mi_ssd_backend_create(NULL, BYTES, 16384, MI_SSD_VERIFY_ON_READ, &ssd) == MI_OK);
    mi_storage_config cfg;
    mi_storage_config_default(&cfg);
    cfg.block_bytes = BYTES;
    cfg.cpu_bytes = SLOTS * BYTES;
    cfg.ssd_bytes = 16384;
    assert(mi_tiered_storage_create(cfg, (mi_storage_backends){.cpu = cpu, .ssd = ssd}, &storage) == MI_OK);
    assert(mi_paged_kv_create(storage, 2, 2, BYTES, "lifecycle", &cache) == MI_OK);
    assert(mi_kv_sequence_create(cache, &s) == MI_OK);
    assert(mi_kv_sequence_append_page(s, MI_LOCATION_CPU) == MI_OK);
    assert(mi_kv_sequence_append_page(s, MI_LOCATION_CPU) == MI_OK);
    write_layer(s, 0, 0);
    write_layer(s, 0, 1);
    write_layer(s, 1, 0);
    assert(mi_kv_sequence_publish(s, tokens, 4, 2) != MI_OK);
    size_t tokens_found;
    assert(mi_paged_kv_match(cache, tokens, 4, &found, &tokens_found) == MI_OK && tokens_found == 0);
    mi_kv_sequence_destroy(found);
    assert(mi_kv_sequence_truncate(s, 1) == MI_OK && mi_kv_sequence_page_count(s) == 1);
    assert(mi_kv_sequence_publish(s, tokens, 4, 1) == MI_OK);
    assert(mi_paged_kv_match(cache, tokens, 4, &found, &tokens_found) == MI_OK && tokens_found == 2);
    mi_kv_sequence_destroy(found);

    mi_page_ref held, clone;
    assert(mi_kv_sequence_retain_page(s, 0, &held) == MI_OK);
    assert(mi_page_ref_clone(&held, &clone) == MI_OK);
    mi_block_id original[2] = {mi_page_ref_block(&held, 0), mi_page_ref_block(&held, 1)};
    mi_page_lease reading;
    assert(mi_paged_kv_acquire(s, 0, 0, MI_ACCESS_READ, NULL, &reading) == MI_OK);
    unsigned char bytes[BYTES];
    mi_block_meta meta;
    assert(mi_storage_read_snapshot(storage, original[0], 1, bytes, BYTES, NULL, &meta) == MI_OK);
    assert(meta.version == 1 && meta.id == original[0] && bytes[BYTES - 1] == 1);
    assert(mi_storage_read_snapshot(storage, original[0], 2, bytes, BYTES, NULL, &meta) == MI_ENOENT);
    assert(mi_storage_erase(storage, original[0]) == MI_EBUSY);
    assert(mi_kv_sequence_truncate(s, 0) == MI_OK);
    assert(mi_paged_kv_evict_prefixes(cache) == MI_OK);
    mi_page_ref_release(&held);
    assert(mi_page_lease_finish(&reading) == MI_OK);
    assert(mi_storage_migrate(storage, original[0], MI_LOCATION_SSD) == MI_OK);
    assert(mi_storage_read_snapshot(storage, original[0], 1, bytes, BYTES, NULL, &meta) == MI_OK);
    assert(meta.location == MI_LOCATION_SSD && bytes[BYTES - 1] == 1);
    mi_page_ref_release(&clone);
    assert(mi_storage_read_snapshot(storage, original[0], 1, bytes, BYTES, NULL, &meta) == MI_ENOENT);

    assert(mi_kv_sequence_append_page(s, MI_LOCATION_CPU) == MI_OK);
    write_layer(s, 0, 0);
    write_layer(s, 0, 1);
    assert(mi_kv_sequence_publish(s, tokens, 4, 1) == MI_OK);
    assert(mi_kv_sequence_retain_page(s, 0, &held) == MI_OK);
    for (size_t layer = 0; layer < 2; ++layer) {
        mi_block_id new_id = mi_page_ref_block(&held, layer);
        mi_block_id old_id = (uint32_t)new_id == (uint32_t)original[0] ? original[0] : original[1];
        assert((uint32_t)new_id == (uint32_t)old_id && (new_id >> 32) > (old_id >> 32));
        assert(mi_storage_read_snapshot(storage, old_id, 1, bytes, BYTES, NULL, &meta) == MI_ENOENT);
    }

    mi_page_ref_release(&held);
    mi_kv_sequence_destroy(s);
    mi_paged_kv_destroy(cache);
    mi_tiered_storage_destroy(storage);
    mi_backend_destroy(ssd);
    mi_backend_destroy(cpu);
    mi_mempool_destroy(pool);
    puts("snapshot, page reference, suffix rollback, and generation reuse: PASS");
    return 0;
}
