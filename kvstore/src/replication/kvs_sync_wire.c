/*
 * 复制协议层。显式编码全量和增量同步消息。
 * 连接两端各自拥有缓冲区。实际收发在复制状态锁之外。
 */
#include "kvs_sync.h"
#include "kvs_allocator.h"
#include "kvs_format.h"
#include "kvs_replication.h"

#include <arpa/inet.h>
#include <string.h>

#define KVS_SYNC_MAGIC 0x4b565331U

static int valid(unsigned int type, size_t length) {
	if (type == KVS_SYNC_PING || type == KVS_SYNC_ACK)
		return length == 0;
	if (!length)
		return 0;
	if (type == KVS_SYNC_SNAPSHOT)
		return length <= kvs_replication_config()->snapshot_bytes;
	return type == KVS_SYNC_UPDATE && length <= KVS_REPLICATION_MAX_COMMAND_SIZE;
}

int kvs_sync_send(kvs_transport_t *io, const kvs_sync_frame_t *frame) {
	unsigned int timeout = kvs_replication_config()->timeout_ms;
	uint32_t header[6];
	if (!valid(frame->type, frame->length) || frame->length > UINT32_MAX)
		return -1;
	header[0] = htonl(KVS_SYNC_MAGIC);
	header[1] = htonl(frame->type);
	header[2] = htonl((uint32_t)frame->length);
	header[3] = htonl(kvs_crc32(frame->data, frame->length));
	header[4] = htonl((uint32_t)(frame->sequence >> 32));
	header[5] = htonl((uint32_t)frame->sequence);
	if (kvs_transport_write(io, header, sizeof(header), timeout))
		return -1;
	return kvs_transport_write(io, frame->data, frame->length, timeout);
}

int kvs_sync_receive(kvs_transport_t *io, kvs_sync_frame_t *frame) {
	unsigned int timeout = kvs_replication_config()->timeout_ms;
	uint32_t header[6];
	memset(frame, 0, sizeof(*frame));
	if (kvs_transport_read(io, header, sizeof(header), timeout))
		return -1;
	frame->type = ntohl(header[1]);
	frame->length = ntohl(header[2]);
	frame->sequence = ((uint64_t)ntohl(header[4]) << 32) | ntohl(header[5]);
	if (ntohl(header[0]) != KVS_SYNC_MAGIC || !valid(frame->type, frame->length))
		return -1;
	if (frame->length) {
		frame->data = kvs_malloc(frame->length + 1);
		if (!frame->data)
			return -1;
		if (kvs_transport_read(io, frame->data, frame->length, timeout))
			goto failed;
		frame->data[frame->length] = 0;
	}
	if (kvs_crc32(frame->data, frame->length) != ntohl(header[3]))
		goto failed;
	return 0;
failed:
	kvs_free(frame->data);
	frame->data = NULL;
	return -1;
}

int kvs_sync_ack(kvs_transport_t *io, uint64_t sequence) {
	kvs_sync_frame_t frame = {.type = KVS_SYNC_ACK, .sequence = sequence};
	return kvs_sync_send(io, &frame);
}

int kvs_sync_expect_ack(kvs_transport_t *io, uint64_t sequence) {
	kvs_sync_frame_t frame;
	if (kvs_sync_receive(io, &frame))
		return -1;
	int result = frame.type == KVS_SYNC_ACK && frame.sequence == sequence ? 0 : -1;
	kvs_free(frame.data);
	return result;
}
