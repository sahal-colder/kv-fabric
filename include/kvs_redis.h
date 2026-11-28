/*
 * 命令层。将 Redis 兼容命令映射到通用 Store。
 * 请求与回复归连接。先取 Store 锁，再进入引擎或复制元数据。
 */
#ifndef KVS_REDIS_H
#define KVS_REDIS_H

#include <stddef.h>

#include "kvs_resp.h"

int kvs_resp_execute_buffer(kvs_resp_arg_t *args, int argc, kvs_resp_buffer *reply);

int kvs_resp_execute_command(
	kvs_resp_arg_t *args,
	int argc,
	char *response,
	size_t *used
);

#endif
