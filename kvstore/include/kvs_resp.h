/*
 * 协议层的 RESP 请求和回复接口。
 * 连接借出输入缓冲区，回复由调用方消费。解析器无独立元数据锁。
 */
#ifndef KVS_RESP_H
#define KVS_RESP_H

#include <stddef.h>

#define KVS_RESP_MAX_ARGS 128

typedef struct kvs_resp_arg_s {
	const char *data;
	size_t length;
} kvs_resp_arg_t;

/* Build one RESP array record. The caller owns the allocated record. */
int kvs_resp_build_record(
	const char *const args[],
	int argc,
	char **record,
	size_t *record_size
);

int kvs_resp_append(
	char *response,
	size_t *used,
	const char *format,
	...
);
int kvs_resp_append_bulk(
	char *response,
	size_t *used,
	const char *value,
	size_t length
);
int kvs_resp_append_value(
	char *response,
	size_t *used,
	const char *value
);
int kvs_resp_arg_equals(
	const kvs_resp_arg_t *arg,
	const char *name
);

#endif
