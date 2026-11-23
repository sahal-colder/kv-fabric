/*
 * 网络层。epoll 就绪事件驱动 TCP 连接。
 * 循环拥有描述符和连接对象。Store 锁范围在命令执行层。
 */
#define _GNU_SOURCE
#include "kvs_network.h"
#include "kvs_protocol.h"
#include "kvs_runtime.h"

#include <errno.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

static int wait_for(int epoll, int op, int fd, kvs_connection_t *conn, uint32_t flags) {
	struct epoll_event event = {.events = flags, .data.ptr = conn};
	return epoll_ctl(epoll, op, fd, &event);
}

static int service_client(int epoll, kvs_connection_t *conn, uint32_t events) {
	int writing = conn->wlength != 0;
	if (events & (EPOLLERR | EPOLLHUP))
		return -1;

	if (!writing && (events & EPOLLIN)) {
		if (kvs_connection_reserve_input(conn))
			return -1;
		ssize_t count = recv(conn->fd, conn->rbuffer + conn->rlength,
		                     conn->rsize - conn->rlength, 0);
		if (count < 0 && (errno == EAGAIN || errno == EINTR))
			return 0;
		if (count <= 0)
			return -1;
		conn->rlength += (size_t)count;
		if (kvs_protocol_process(conn))
			return -1;
	}

	if (conn->wlength) {
		ssize_t count = send(conn->fd, conn->wbuffer + conn->woffset,
		                     conn->wlength, MSG_NOSIGNAL);
		if (count < 0 && errno != EAGAIN && errno != EINTR)
			return -1;
		if (count == 0)
			return -1;
		if (count > 0) {
			conn->woffset += (size_t)count;
			conn->wlength -= (size_t)count;
		}
		if (!conn->wlength && (conn->close_after_write || kvs_protocol_process(conn)))
			return -1;
	}
	if (writing != (conn->wlength != 0))
		return wait_for(epoll, EPOLL_CTL_MOD, conn->fd, conn,
		                conn->wlength ? EPOLLOUT : EPOLLIN);
	return 0;
}

int reactor_start_protocol(unsigned short port) {
	int listener = kvs_tcp_listen(port, SOCK_NONBLOCK);
	if (listener < 0) {
		perror("reactor listen");
		return -1;
	}
	int epoll = epoll_create1(EPOLL_CLOEXEC);
	kvs_connection_t *clients = NULL;
	int result = -1;
	if (epoll < 0 || wait_for(epoll, EPOLL_CTL_ADD, listener, NULL, EPOLLIN)) {
		perror("reactor epoll setup");
		goto done;
	}

	while (!atomic_load_explicit(&kvs_shutdown_requested, memory_order_relaxed)) {
		struct epoll_event events[128];
		int ready = epoll_wait(epoll, events, 128, 250);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0) {
			perror("epoll_wait");
			goto done;
		}
		for (int i = 0; i < ready; i++) {
			kvs_connection_t *conn = events[i].data.ptr;
			if (conn) {
				if (service_client(epoll, conn, events[i].events)) {
					/* A replication worker can retain a duplicate of fd. */
					if (epoll_ctl(epoll, EPOLL_CTL_DEL, conn->fd, NULL)) {
						perror("reactor detach");
						goto done;
					}
					kvs_connection_remove(conn);
				}
				continue;
			}

			int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
			if (fd < 0) {
				if (errno == EAGAIN || errno == EINTR || errno == ECONNABORTED)
					continue;
				perror("reactor accept");
				goto done;
			}
			conn = kvs_tcp_nodelay(fd) ? NULL : kvs_connection_add(&clients, fd);
			if (!conn) {
				close(fd);
				continue;
			}
			if (wait_for(epoll, EPOLL_CTL_ADD, fd, conn, EPOLLIN))
				kvs_connection_remove(conn);
		}
	}
	result = 0;
done:
	if (epoll >= 0)
		close(epoll);
	while (clients)
		kvs_connection_remove(clients);
	close(listener);
	return result;
}
