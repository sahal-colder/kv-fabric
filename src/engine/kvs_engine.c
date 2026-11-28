/*
 * 引擎层。将命令分发到索引，并按写顺序提交 AOF 与复制事件。
 * 索引拥有值，输入命令借用。Store 写锁在持久化或复制队列锁之前。
 */
#include "kvs_engine.h"
#include "kvs_format.h"
#include "kvs_master.h"
#include "kvs_persistence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <strings.h>

static int resp_engine = KVS_FORMAT_ENGINE_ARRAY;
static const char *const engine_names[] = {"array", "rbtree", "hash", "skiplist"};

const char *kvs_resp_engine_name(void) {
	return engine_names[resp_engine];
}

int kvs_resp_select_engine(const char *name) {
	if (!strcasecmp(name, "skiptable"))
		name = "skiplist";
	for (size_t i = 0; i < sizeof(engine_names) / sizeof(*engine_names); i++) {
		if (!strcasecmp(name, engine_names[i])) {
			resp_engine = (int)i;
			return 0;
		}
	}
	return -1;
}

static const char *const commands[][5] = {
    {"SET", "MOD", "SET", "DEL", "FLUSH"},
    {"RSET", "RMOD", "RSET", "RDEL", "RFLUSH"},
    {"HSET", "HMOD", "HSET", "HDEL", "HFLUSH"},
    {"SSET", "SMOD", "SSET", "SDEL", "SFLUSH"}
};

static void publish_write(int engine, kvs_mutation_t operation, const char *key, const char *value,
                          kvs_event_source_t source) {
	kvs_write_event_t event = {commands[engine][operation], key ? key : "", value, source};
	/* A failed log append after a mutation leaves RAM ahead of disk. Stop before
	 * this write can be acknowledged, observed by other clients, or replicated. */
	if (kvs_persistence_append(&event)) {
		fprintf(stderr, "AOF append failed for %s; stopping before acknowledgement\n", event.cmd);
		_Exit(EXIT_FAILURE);
	}
	kvs_master_publish(&event);
}

int kvs_engine_mutate_locked(int engine, kvs_mutation_t operation,
                             const char *key, const char *value,
                             kvs_event_source_t source) {
	int result = kvs_store_mutate(&kvs_store, engine,
	                              operation == KVS_UPSERT ? KVS_INSERT : operation,
	                              key, value);
	if (operation == KVS_UPSERT && result == 1) {
		operation = KVS_UPDATE;
		result = kvs_store_mutate(&kvs_store, engine, operation, key, value);
	}
	if (result)
		return result;

	publish_write(engine, operation, key, value, source);
	return 0;
}

int kvs_resp_set_many(const char *const *keys, const char *const *values, size_t count, int only_missing) {
    int result = kvs_store_mset(&kvs_store, resp_engine, keys, values, count, only_missing);
    if (result)
        return result;
    for (size_t i = 0; i < count; ++i)
        publish_write(resp_engine, KVS_UPSERT, keys[i], values[i], KVS_EVENT_SOURCE_CLIENT);
    return 0;
}

int kvs_engine_replay(const char *command, const char *key, const char *value) {
	int engine = kvs_format_command_engine(command);
	if (engine < 0)
		return -1;
	const char *name = engine == KVS_FORMAT_ENGINE_ARRAY ? command : command + 1;
	kvs_mutation_t operation = !strcmp(name, "FLUSH") ? KVS_CLEAR :
	                           !strcmp(name, "DEL") ? KVS_DELETE : KVS_UPSERT;
	kvs_store_write_lock();
	int result = kvs_engine_mutate_locked(engine, operation, key, value,
	                                      KVS_EVENT_SOURCE_REPLICATION_INCREMENTAL);
	kvs_store_unlock();
	return result > 0 && operation == KVS_DELETE ? 0 : result;
}

int kvs_engine_load(const char *filename) {
	kvs_store_write_lock();
	int result = kvs_persistence_load(filename);
	if (!result)
		kvs_master_resync();
	kvs_store_unlock();
	return result;
}

const char *kvs_resp_engine_get(const char *key) {
	return kvs_store_get(&kvs_store, resp_engine, key);
}

int kvs_resp_engine_delete(const char *key) {
	return kvs_engine_mutate_locked(resp_engine, KVS_DELETE, key, NULL,
	                                KVS_EVENT_SOURCE_CLIENT);
}

int kvs_resp_set_string(const char *key, const char *value) {
	return kvs_engine_mutate_locked(resp_engine, KVS_UPSERT, key, value,
	                                KVS_EVENT_SOURCE_CLIENT);
}

int kvs_resp_key_count(void) {
	return kvs_store_count(&kvs_store, resp_engine);
}

int kvs_resp_engine_clear(void) {
	return kvs_engine_mutate_locked(resp_engine, KVS_CLEAR, NULL, NULL,
	                                KVS_EVENT_SOURCE_CLIENT);
}
