/*
 * 网络层。维护单个连接的累积输入、解析进度和输出队列。
 * 连接对象拥有缓冲区，所属事件循环串行访问。
 */
#ifndef KVS_CONNECTION_H
#define KVS_CONNECTION_H

#include <stddef.h>

#define KVS_MAX_REQUEST (64U * 1024U)
#define KVS_OUTPUT_BATCH (16U * 1024U)

typedef struct kvs_connection {
	int fd;
	struct kvs_connection *next, **previous;
	int close_after_write;
	char *rbuffer;
	size_t rlength, rsize;
	char *wbuffer;
	size_t woffset, wlength, wsize;
} kvs_connection_t;

/* Prepare writable space before issuing recv, including an async recv. */
int kvs_connection_reserve_input(kvs_connection_t *conn);
kvs_connection_t *kvs_connection_add(kvs_connection_t **head, int fd);
void kvs_connection_remove(kvs_connection_t *conn);
void kvs_connection_destroy(kvs_connection_t *conn);
int kvs_connection_queue_output(kvs_connection_t *conn, const char *data, size_t length);

#endif
