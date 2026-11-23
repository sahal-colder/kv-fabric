/*
 * 持久化层的可选 MySQL 审计队列。
 * 队列拥有事件副本，后台线程执行数据库操作。Store 写锁在队列锁之前。
 */
#ifndef KVS_MYSQL_H
#define KVS_MYSQL_H

#include "kvs_mutation.h"

#if ENABLE_MYSQL
int kvs_mysql_start(void);
void kvs_mysql_stop(void);
int kvs_mysql_append(const char *cmd, const char *key, const char *value,
	kvs_event_source_t source);
#endif

#endif
