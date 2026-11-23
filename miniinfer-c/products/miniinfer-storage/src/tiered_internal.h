/* Logical Storage helpers. Callers may hold Trie, never Backend. Snapshots release locks before I/O. */
#ifndef MINIINFER_TIERED_INTERNAL_H
#define MINIINFER_TIERED_INTERNAL_H
#include "miniinfer/tiered_storage.h"
#include "miniinfer/device_transfer.h"

/* 发布前检查所有层的块均已初始化、驻留且无租约或迁移。 */
mi_status mi_storage_blocks_ready(mi_tiered_storage *storage, const mi_block_id *ids, size_t count);
mi_status mi_storage_read_snapshot_event(mi_tiered_storage *storage, mi_block_id id, uint64_t version,
                                         void *dst, size_t capacity, mi_stream stream, mi_event event,
                                         mi_block_meta *out);

#endif
