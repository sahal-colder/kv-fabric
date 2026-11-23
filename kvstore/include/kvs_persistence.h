/*
 * 持久化层。编码、读取并原子替换快照。
 * 文件和临时 Store 由操作持有。序列化的共享 Store 由其读写锁保护。
 */
#ifndef KVS_PERSISTENCE_H
#define KVS_PERSISTENCE_H

#include <stddef.h>

#include "kvs_mutation.h"

int kvs_persistence_start(void);
int kvs_persistence_stop(void);
int kvs_persistence_save(const char *filename);
/* Caller holds the write lock for load and append. */
int kvs_persistence_load(const char *filename);
int kvs_persistence_append(const kvs_write_event_t *event);

/* Caller holds the store lock across export and replica registration. */
int kvs_snapshot_export_locked(char **data, size_t *length, size_t limit);
int kvs_snapshot_import(const void *data, size_t length);

#endif
