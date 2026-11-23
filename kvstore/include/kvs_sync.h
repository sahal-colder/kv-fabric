/*
 * 复制协议层。描述握手、全量快照和有序增量消息。
 * 协议缓冲区随连接或读写调用存在。无共享 Store 锁嵌套。
 */
#ifndef KVS_SYNC_H
#define KVS_SYNC_H

#include <stdint.h>
#include "kvs_transport.h"

enum { KVS_SYNC_SNAPSHOT = 1, KVS_SYNC_UPDATE, KVS_SYNC_PING, KVS_SYNC_ACK };
typedef struct {
	unsigned int type;
	uint64_t sequence;
	char *data;
	size_t length;
} kvs_sync_frame_t;

int kvs_sync_send(kvs_transport_t *io, const kvs_sync_frame_t *frame);
int kvs_sync_receive(kvs_transport_t *io, kvs_sync_frame_t *frame);
int kvs_sync_ack(kvs_transport_t *io, uint64_t sequence);
int kvs_sync_expect_ack(kvs_transport_t *io, uint64_t sequence);

#endif
