/*
 * 引擎层。将命令分发到索引，并按写顺序提交 AOF 与复制事件。
 * 索引拥有值，输入命令借用。Store 写锁在持久化或复制队列锁之前。
 */
#ifndef KVS_ENGINE_H
#define KVS_ENGINE_H

#include "kvs_store.h"

int kvs_resp_select_engine(const char *name);
const char *kvs_resp_engine_name(void);

const char *kvs_resp_engine_get(const char *key);
int kvs_resp_engine_delete(const char *key);
int kvs_resp_set_many(const char *const *keys, const char *const *values, size_t count, int only_missing);
int kvs_resp_set_string(const char *key, const char *value);
int kvs_resp_key_count(void);
int kvs_resp_engine_clear(void);

/* The caller holds the write lock through storage, AOF and publication. */
int kvs_engine_mutate_locked(int engine, kvs_mutation_t operation,
                             const char *key, const char *value,
                             kvs_event_source_t source);
int kvs_engine_replay(const char *command, const char *key, const char *value);
int kvs_engine_load(const char *filename);

#endif
