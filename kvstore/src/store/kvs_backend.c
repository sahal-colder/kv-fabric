/*
 * 持久化层。路由快照、AOF 与 MySQL 审计后端。
 * 各后端管理文件和队列。调用写日志时由调用方先持 Store 写锁。
 */
#include "kvs_persistence_internal.h"
#include "kvs_store.h"
#include "kvs_mysql.h"
#include "kvs_config.h"

int kvs_persistence_start(void) {
	const kvs_config_t *c = kvs_config_get();
	if (c->persistence == KVS_PERSISTENCE_INCREMENTAL && kvs_incremental_start())
		return -1;
#if ENABLE_MYSQL
	if (c->mysql_enabled && kvs_mysql_start()) {
		if (c->persistence == KVS_PERSISTENCE_INCREMENTAL)
			kvs_incremental_stop();
		return -1;
	}
#endif
	return 0;
}

int kvs_persistence_stop(void) {
#if ENABLE_MYSQL
	if (kvs_config_get()->mysql_enabled)
		kvs_mysql_stop();
#endif
	return kvs_config_get()->persistence == KVS_PERSISTENCE_INCREMENTAL ? kvs_incremental_stop() : 0;
}

int kvs_persistence_save(const char *filename) {
	int mode = kvs_config_get()->persistence;
	if (mode == KVS_PERSISTENCE_FULL)
		return kvs_save(filename);
	if (mode != KVS_PERSISTENCE_INCREMENTAL)
		return -1;
	kvs_store_read_lock();
	int result = kvs_incremental_save();
	kvs_store_unlock();
	return result;
}

int kvs_persistence_load(const char *filename) {
	int mode = kvs_config_get()->persistence;
	if (mode == KVS_PERSISTENCE_FULL)
		return kvs_load(filename);
	return mode == KVS_PERSISTENCE_INCREMENTAL ? kvs_incremental_load() : -1;
}

int kvs_persistence_append(const kvs_write_event_t *event) {
	if (kvs_config_get()->persistence == KVS_PERSISTENCE_INCREMENTAL &&
	    kvs_incremental_append(event->cmd, event->key, event->value))
		return -1;
#if ENABLE_MYSQL
	if (kvs_config_get()->mysql_enabled &&
	    kvs_mysql_append(event->cmd, event->key, event->value, event->source))
		fprintf(stderr, "MySQL audit queue rejected %s\n", event->cmd);
#endif
	return 0;
}
