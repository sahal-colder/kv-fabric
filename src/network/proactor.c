/*
 * 网络层。io_uring 完成事件驱动连接状态机。
 * 未完成请求保留缓冲区，事件循环串行提交和收割完成项。
 */
#include "kvs_network.h"
#include "kvs_protocol.h"
#include "kvs_runtime.h"

#include <errno.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RING_ENTRIES 1024
#define CQ_BATCH 64

static int submit(struct io_uring *ring) {
	while (io_uring_sq_ready(ring)) {
		int result = io_uring_submit(ring);
		if (result == -EINTR)
			continue;
		if (result <= 0)
			return result ? result : -EIO;
	}
	return 0;
}

static int queue_accept(struct io_uring *ring, int listener) {
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	if (!sqe)
		return -1;
	io_uring_prep_accept(sqe, listener, NULL, NULL, SOCK_CLOEXEC);
	io_uring_sqe_set_data(sqe, NULL);
	return 0;
}

static int queue_client(struct io_uring *ring, kvs_connection_t *conn) {
	if (!conn->wlength && kvs_connection_reserve_input(conn))
		return -1;
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	if (!sqe)
		return -1;
	if (conn->wlength) {
		io_uring_prep_send(sqe, conn->fd, conn->wbuffer + conn->woffset,
		                   conn->wlength, MSG_NOSIGNAL);
	} else {
		io_uring_prep_recv(sqe, conn->fd, conn->rbuffer + conn->rlength,
		                   conn->rsize - conn->rlength, 0);
	}
	io_uring_sqe_set_data(sqe, conn);
	return 0;
}

static int complete_client(struct io_uring *ring, kvs_connection_t *conn, int count) {
	/* Only one operation owns this connection or either buffer at a time. */
	if (count == -EINTR)
		return queue_client(ring, conn);
	if (count <= 0)
		return -1;
	if (conn->wlength) {
		conn->woffset += (size_t)count;
		conn->wlength -= (size_t)count;
	} else {
		conn->rlength += (size_t)count;
	}
	if (!conn->wlength && (conn->close_after_write || kvs_protocol_process(conn)))
		return -1;
	return queue_client(ring, conn);
}

int proactor_start_protocol(unsigned short port) {
	int listener = kvs_tcp_listen(port, 0);
	if (listener < 0) {
		perror("proactor listen");
		return -1;
	}
	struct io_uring ring;
	int status = io_uring_queue_init(RING_ENTRIES, &ring, 0);
	if (status < 0) {
		fprintf(stderr, "io_uring setup: %s\n", strerror(-status));
		close(listener);
		return -1;
	}

	kvs_connection_t *clients = NULL;
	unsigned pending = 0;
	int result = -1;
	if (queue_accept(&ring, listener))
		goto done;
	pending++;
	while (!atomic_load_explicit(&kvs_shutdown_requested, memory_order_relaxed)) {
		status = submit(&ring);
		if (status < 0)
			goto ring_failure;
		struct __kernel_timespec timeout = {.tv_nsec = 250000000};
		struct io_uring_cqe *cqe;
		status = io_uring_wait_cqe_timeout(&ring, &cqe, &timeout);
		if (status == -EINTR || status == -ETIME)
			continue;
		if (status < 0)
			goto ring_failure;

		/* At most two new SQEs per CQE, so a batch cannot fill the SQ. */
		for (int i = 0; i < CQ_BATCH; i++) {
			kvs_connection_t *conn = io_uring_cqe_get_data(cqe);
			int count = cqe->res;
			io_uring_cqe_seen(&ring, cqe);
			pending--;
			if (conn) {
				if (complete_client(&ring, conn, count))
					kvs_connection_remove(conn);
				else
					pending++;
			} else {
				if (count >= 0) {
					conn = kvs_tcp_nodelay(count) ? NULL : kvs_connection_add(&clients, count);
					if (!conn)
						close(count);
					else if (queue_client(&ring, conn))
						kvs_connection_remove(conn);
					else
						pending++;
				} else if (count != -EINTR && count != -ECONNABORTED && count != -EAGAIN) {
					fprintf(stderr, "proactor accept: %s\n", strerror(-count));
					goto done;
				}
				if (queue_accept(&ring, listener))
					goto done;
				pending++;
			}
			if (i + 1 == CQ_BATCH)
				break;
			status = io_uring_peek_cqe(&ring, &cqe);
			if (status == -EAGAIN)
				break;
			if (status < 0)
				goto ring_failure;
		}
	}
	result = 0;

done:
	/* Wake blocked operations, then wait for every CQE before freeing buffers.
	 * A successful accept racing shutdown still returns an owned descriptor. */
	shutdown(listener, SHUT_RDWR);
	for (kvs_connection_t *conn = clients; conn; conn = conn->next)
		shutdown(conn->fd, SHUT_RDWR);
	status = submit(&ring);
	if (status < 0)
		goto ring_failure;
	while (pending) {
		struct io_uring_cqe *cqe;
		status = io_uring_wait_cqe(&ring, &cqe);
		if (status == -EINTR)
			continue;
		if (status < 0)
			goto ring_failure;
		if (!io_uring_cqe_get_data(cqe) && cqe->res >= 0)
			close(cqe->res);
		io_uring_cqe_seen(&ring, cqe);
		pending--;
	}
	io_uring_queue_exit(&ring);
	while (clients)
		kvs_connection_remove(clients);
	close(listener);
	return result;

ring_failure:
	fprintf(stderr, "io_uring ring failure: %s\n", strerror(-status));
	/* A broken ring cannot confirm that kernel buffer users have completed. */
	_Exit(EXIT_FAILURE);
}
