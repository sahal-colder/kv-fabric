/*
 * 持久化内部接口。定义快照和 AOF 各自的读写职责。
 * 文件由对应后端持有。需要访问共享数据时先取得 Store 锁。
 */
#ifndef KVS_PERSISTENCE_INTERNAL_H
#define KVS_PERSISTENCE_INTERNAL_H

#include "kvs_persistence.h"

int kvs_save(const char *filename);
int kvs_load(const char *filename);
int kvs_incremental_start(void);
int kvs_incremental_stop(void);
int kvs_incremental_save(void);
int kvs_incremental_load(void);
int kvs_incremental_replace(const void *data, size_t length);
int kvs_incremental_append(const char *cmd, const char *key, const char *value);

#endif
