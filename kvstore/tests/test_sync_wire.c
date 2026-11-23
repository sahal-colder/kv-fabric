/*
 * 通用 KVStore 回归层，覆盖 sync wire。
 * 夹具拥有本地数据或连接。业务锁由被测代码管理。
 */
#include "kvs_sync.h"
#include "kvs_allocator.h"
#include "kvs_replication.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
	kvs_replication_config_t config = {.timeout_ms = 2000, .snapshot_bytes = 4096};
	int pair[2];
	assert(!kvs_allocator_init());
	assert(!kvs_replication_configure(&config));
	assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
	kvs_transport_t writer = {.fd = pair[0]}, reader = {.fd = pair[1]};
	char data[] = "a\0b\r\n";
	kvs_sync_frame_t sent = {KVS_SYNC_UPDATE, UINT64_C(0x123456789abcdef0), data, sizeof(data)},
	                 got;
	assert(!kvs_sync_send(&writer, &sent));
	assert(!kvs_sync_receive(&reader, &got));
	assert(got.type == sent.type && got.sequence == sent.sequence && got.length == sent.length &&
	       !memcmp(got.data, data, sizeof(data)));
	kvs_free(got.data);
	assert(!kvs_sync_ack(&writer, sent.sequence));
	assert(!kvs_sync_expect_ack(&reader, sent.sequence));
	/* A checksummed frame with corrupt payload is rejected before application. */
	uint32_t bad[6] = {htonl(0x4b565331), htonl(KVS_SYNC_UPDATE), htonl(1), htonl(0), 0, 0};
	assert(!kvs_transport_write(&writer, bad, sizeof(bad), 2000));
	assert(!kvs_transport_write(&writer, "x", 1, 2000));
	assert(kvs_sync_receive(&reader, &got) < 0 && got.data == NULL);
	bad[2] = htonl(KVS_REPLICATION_MAX_COMMAND_SIZE + 1);
	assert(!kvs_transport_write(&writer, bad, sizeof(bad), 2000));
	assert(kvs_sync_receive(&reader, &got) < 0 && got.data == NULL);
	assert(!kvs_transport_write(&writer, bad, 5, 2000));
	kvs_transport_close(&writer);
	assert(kvs_sync_receive(&reader, &got) < 0);
	kvs_transport_close(&reader);
	kvs_allocator_destroy();
	puts("sync wire: PASS");
	return 0;
}
