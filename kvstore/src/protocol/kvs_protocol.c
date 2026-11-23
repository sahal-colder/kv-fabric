/*
 * 协议层。统一文本或 RESP 请求的执行入口。
 * 解析只借用输入，回复按上层协议分配。Store 锁用于命令语义，网络层发送回复。
 */
#include "kvs_protocol.h"
#include "kvs_format.h"
#include "kvs_master.h"
#include "kvs_redis.h"
#include "kvs_replication.h"
#include "kvs_slave.h"

#include <stdio.h>
#include <string.h>

int kvs_protocol_process(kvs_connection_t *conn) {
	conn->woffset = 0;
	if (!conn->rlength)
		return 0;
	const kvs_replication_config_t *replication = kvs_replication_config();
	if (replication->role == KVS_ROLE_MASTER && replication->transport == KVS_SYNC_TCP &&
	    !replication->listen_port) {
		static const char hello[] = KVS_SLAVE_HANDSHAKE;
		size_t length = sizeof(hello) - 1;
		size_t prefix = conn->rlength < length ? conn->rlength : length;
		if (!memcmp(conn->rbuffer, hello, prefix)) {
			if (conn->rlength < length)
				return 0;
			return conn->rlength == length && !kvs_master_register_slave(conn->fd) ? 1 : -1;
		}
	}

	size_t offset = 0;
	while (offset < conn->rlength && conn->wlength < KVS_OUTPUT_BATCH) {
		char response[BUFFER_LENGTH];
		int length;
		if (conn->rbuffer[offset] == '*') {
			kvs_resp_arg_t args[KVS_RESP_MAX_ARGS];
			int argc;
			size_t record_length;
			int status = kvs_format_parse_resp_record(conn->rbuffer, conn->rlength, &offset,
			                                          args, &argc, &record_length);
			if (!status)
				break;
			if (status < 0) {
				length = sprintf(response, "-ERR bad resp request\r\n");
				conn->close_after_write = 1;
			} else {
				/* The store is string based. Terminate only a complete frame. */
				int binary = 0;
				int binary_allowed = kvs_resp_arg_equals(&args[0], "ECHO") ||
				                     kvs_resp_arg_equals(&args[0], "PING");
				for (int i = 0; i < argc; i++) {
					if (!binary_allowed && memchr(args[i].data, 0, args[i].length))
						binary = 1;
					((char *)args[i].data)[args[i].length] = '\0';
				}
				size_t used = 0;
				if (binary)
					length = sprintf(response, "-ERR embedded NUL is unsupported\r\n");
				else if (kvs_resp_execute_command(args, argc, response, &used))
					length = sprintf(response, "-ERR command failed or reply too large\r\n");
				else
					length = (int)used;
			}
		} else {
			char *line = conn->rbuffer + offset;
			char *end = memchr(line, '\n', conn->rlength - offset);
			if (!end)
				break;
			offset = (size_t)(end - conn->rbuffer) + 1;
			*end = '\0';
			length = memchr(line, 0, (size_t)(end - line)) ?
			         sprintf(response, "ERROR\r\n") : kvs_text_command(line, response);
		}
		if (kvs_connection_queue_output(conn, response, (size_t)length))
			return -1;
		if (conn->close_after_write)
			return 0;
	}
	if (offset) {
		conn->rlength -= offset;
		memmove(conn->rbuffer, conn->rbuffer + offset, conn->rlength);
	}
	return conn->rlength == KVS_MAX_REQUEST && !conn->wlength ? -1 : 0;
}
