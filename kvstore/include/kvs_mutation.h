/*
 * 引擎层的写操作类型，定义 Store 与持久化共享的变更语义。
 * 值类型不持有内存和锁。
 */
#ifndef KVS_MUTATION_H
#define KVS_MUTATION_H

/* Engine numbers are also the Redis logical database numbers in RESP files. */
#define KVS_FORMAT_ENGINE_UNKNOWN	(-1)
#define KVS_FORMAT_ENGINE_ARRAY		0
#define KVS_FORMAT_ENGINE_RBTREE	1
#define KVS_FORMAT_ENGINE_HASH		2
#define KVS_FORMAT_ENGINE_SKIPTABLE	3

#define KVS_MAX_FIELD_BYTES (16U * 1024U * 1024U)

/* Mutation published by the engine while it still owns the write lock.
 * Source values are stored in the optional MySQL audit table. */
typedef enum kvs_event_source_e {
	KVS_EVENT_SOURCE_CLIENT = 0,
	KVS_EVENT_SOURCE_REPLICATION_INCREMENTAL = 3
} kvs_event_source_t;

typedef struct kvs_write_event_s {
	const char *cmd;
	const char *key;
	const char *value;
	kvs_event_source_t source;
} kvs_write_event_t;

#endif
