/*
 * 网络层。用 NtyCo 协程实现连接接收和等待。
 * 协程拥有自身连接，Store 锁不由协程跨 socket 等待持有。
 */
#include "nty_coroutine.h"

#include "kvs_network.h"
#include "kvs_protocol.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static void client_reader(void *arg) {
	kvs_connection_t conn = {.fd = (int)(intptr_t)arg};
	for (;;) {
		if (kvs_protocol_process(&conn))
			break;
		if (conn.wlength) {
			while (conn.wlength) {
				ssize_t count = send(conn.fd, conn.wbuffer + conn.woffset,
				                     conn.wlength, MSG_NOSIGNAL);
				if (count < 0 && (errno == EAGAIN || errno == EINTR))
					continue;
				if (count <= 0)
					goto done;
				conn.woffset += (size_t)count;
				conn.wlength -= (size_t)count;
			}
			if (conn.close_after_write)
				break;
			continue;
		}
		if (kvs_connection_reserve_input(&conn))
			break;
		ssize_t count = nty_recv(conn.fd, conn.rbuffer + conn.rlength,
		                         conn.rsize - conn.rlength, 0);
		if (count < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (count <= 0)
			break;
		conn.rlength += (size_t)count;
	}
done:
	kvs_connection_destroy(&conn);
	close(conn.fd);
}

static void server(void *arg) {
	int listener = (int)(intptr_t)arg;
	for (;;) {
		int fd = accept(listener, NULL, NULL);
		if (fd < 0) {
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			perror("ntyco accept");
			break;
		}
		nty_coroutine *reader;
		if (kvs_tcp_nodelay(fd) ||
		    nty_coroutine_create(&reader, client_reader, (void *)(intptr_t)fd))
			close(fd);
	}
	close(listener);
}

int ntyco_start_protocol(unsigned short port) {
	int listener = kvs_tcp_listen(port, SOCK_NONBLOCK);
	if (listener < 0) {
		perror("ntyco listen");
		return -1;
	}
	nty_coroutine *acceptor;
	if (nty_coroutine_create(&acceptor, server, (void *)(intptr_t)listener)) {
		close(listener);
		return -1;
	}
	nty_schedule_run();
	return -1; /* The accept coroutine only stops on error. */
}
