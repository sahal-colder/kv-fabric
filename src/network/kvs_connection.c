/*
 * 网络层。维护单个连接的累积输入、解析进度和输出队列。
 * 连接对象拥有缓冲区，所属事件循环串行访问。
 */
#include "kvs_connection.h"
#include "kvs_allocator.h"
#include "kvs_network.h"
#include "kvs_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

static int grow(char **buffer, size_t *capacity, size_t needed) {
	if (needed <= *capacity)
		return 0;
	size_t next = *capacity ? *capacity : BUFFER_LENGTH;
	while (next < needed) {
		if (next > SIZE_MAX / 2) {
			next = needed;
			break;
		}
		next *= 2;
	}
	char *new_buffer = kvs_realloc(*buffer, next);
	if (!new_buffer)
		return -1;
	*buffer = new_buffer;
	*capacity = next;
	return 0;
}

int kvs_connection_reserve_input(kvs_connection_t *conn) {
	if (conn->rlength == KVS_MAX_REQUEST)
		return -1;
	return grow(&conn->rbuffer, &conn->rsize, conn->rlength + 1);
}

void kvs_connection_destroy(kvs_connection_t *conn) {
	kvs_free(conn->rbuffer);
	kvs_free(conn->wbuffer);
}

kvs_connection_t *kvs_connection_add(kvs_connection_t **head, int fd) {
	kvs_connection_t *conn = kvs_calloc(1, sizeof(*conn));
	if (!conn)
		return NULL;
	conn->fd = fd;
	conn->next = *head;
	conn->previous = head;
	if (*head)
		(*head)->previous = &conn->next;
	*head = conn;
	return conn;
}

void kvs_connection_remove(kvs_connection_t *conn) {
	*conn->previous = conn->next;
	if (conn->next)
		conn->next->previous = conn->previous;
	close(conn->fd);
	kvs_connection_destroy(conn);
	kvs_free(conn);
}

int kvs_connection_queue_output(kvs_connection_t *conn, const char *data, size_t length) {
	if (!conn || (length && !data))
		return -1;
	if (!length)
		return 0;
	/* Protocol runs only when the previous batch has drained. */
	if ((length && !data) || length > SIZE_MAX - conn->wlength ||
	    grow(&conn->wbuffer, &conn->wsize, conn->wlength + length))
		return -1;
	memcpy(conn->wbuffer + conn->wlength, data, length);
	conn->wlength += length;
	return 0;
}

int kvs_tcp_listen(unsigned short port, int flags) {
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | flags, 0);
	if (fd < 0)
		return -1;
	int reuse = 1;
	struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
	inet_pton(AF_INET, kvs_config_get()->bind, &address.sin_addr);
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) ||
	    bind(fd, (struct sockaddr *)&address, sizeof(address)) || listen(fd, 1024)) {
		int error = errno;
		close(fd);
		errno = error;
		return -1;
	}
	return fd;
}

int kvs_tcp_nodelay(int fd) {
	int enabled = 1;
	return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
}
