/*
 * 复制网络层。按配置调用 TCP 或 rsockets。
 * 调用方持有连接和收发缓冲区。无内置 Store 锁。
 */
#include "kvs_transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if KVS_ENABLE_RDMA
#include <rdma/rsocket.h>
#define IO(io, fn, ...) ((io)->rdma ? r##fn(__VA_ARGS__) : fn(__VA_ARGS__))
#else
#define IO(io, fn, ...) fn(__VA_ARGS__)
#endif

static int nonblocking(kvs_transport_t *io) {
	int flags = IO(io, fcntl, io->fd, F_GETFL, 0);
	return flags < 0 ? -1 : IO(io, fcntl, io->fd, F_SETFL, flags | O_NONBLOCK);
}

static int open_socket(kvs_transport_t *io, int rdma) {
	io->rdma = rdma;
#if !KVS_ENABLE_RDMA
	if (rdma) {
		errno = ENOTSUP;
		return -1;
	}
#endif
	io->fd = IO(io, socket, AF_INET, SOCK_STREAM, 0);
	if (io->fd < 0)
		return -1;
	if (nonblocking(io)) {
		kvs_transport_close(io);
		return -1;
	}
	if (!rdma) {
		int yes = 1;
		(void)setsockopt(io->fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
		(void)setsockopt(io->fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
	}
	return 0;
}

int kvs_transport_wait(kvs_transport_t *io, short events, unsigned int timeout_ms) {
	struct pollfd p = {.fd = io->fd, .events = events};
	int result;
	do {
		result = IO(io, poll, &p, 1, (int)timeout_ms);
	} while (result < 0 && errno == EINTR);
	if (result == 0)
		errno = ETIMEDOUT;
	if (result <= 0)
		return -1;
	/* POLLIN may accompany HUP while the peer's last bytes are still readable. */
	if (p.revents & events)
		return 0;
	errno = ECONNRESET;
	return -1;
}

int kvs_transport_connect(kvs_transport_t *io, const char *host, unsigned short port, int rdma,
                          unsigned int timeout_ms) {
	struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *addresses;
	char service[6];
	int result = -1;
	io->fd = -1;
	snprintf(service, sizeof(service), "%u", port);
	if (getaddrinfo(host, service, &hints, &addresses))
		return -1;
	for (struct addrinfo *a = addresses; a; a = a->ai_next) {
		int error = 0;
		socklen_t length = sizeof(error);
		if (open_socket(io, rdma))
			continue;
		result = IO(io, connect, io->fd, a->ai_addr, a->ai_addrlen);
		if (result && errno == EINPROGRESS && !kvs_transport_wait(io, POLLOUT, timeout_ms)) {
			result = IO(io, getsockopt, io->fd, SOL_SOCKET, SO_ERROR, &error, &length);
			if (!result && error) {
				errno = error;
				result = -1;
			}
		}
		if (!result)
			break;
		kvs_transport_close(io);
	}
	freeaddrinfo(addresses);
	return result;
}

int kvs_transport_listen(kvs_transport_t *io, const char *host, unsigned short port, int rdma) {
	struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
	int yes = 1;
	if (inet_pton(AF_INET, host, &address.sin_addr) != 1 || open_socket(io, rdma))
		return -1;
	(void)IO(io, setsockopt, io->fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	if (IO(io, bind, io->fd, (struct sockaddr *)&address, sizeof(address)) ||
	    IO(io, listen, io->fd, 64)) {
		kvs_transport_close(io);
		return -1;
	}
	return 0;
}

int kvs_transport_accept(kvs_transport_t *listener, kvs_transport_t *peer,
                         unsigned int timeout_ms) {
	peer->rdma = listener->rdma;
	peer->fd = -1;
	if (kvs_transport_wait(listener, POLLIN, timeout_ms))
		return -1;
	peer->fd = IO(listener, accept, listener->fd, NULL, NULL);
	if (peer->fd < 0)
		return -1;
	if (nonblocking(peer)) {
		kvs_transport_close(peer);
		return -1;
	}
	return 0;
}

static unsigned long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int transfer(kvs_transport_t *io, void *data, size_t bytes, unsigned int timeout_ms,
                    int write) {
	unsigned char *p = data;
	unsigned long long deadline = now_ms() + timeout_ms;
	while (bytes) {
		unsigned long long now = now_ms();
		ssize_t n;
		if (now >= deadline) {
			errno = ETIMEDOUT;
			return -1;
		}
		if (kvs_transport_wait(io, write ? POLLOUT : POLLIN, (unsigned int)(deadline - now)))
			return -1;
		if (write)
			n = IO(io, send, io->fd, p, bytes, MSG_NOSIGNAL);
		else
			n = IO(io, recv, io->fd, p, bytes, 0);
		if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
			continue;
		if (n <= 0)
			return -1;
		p += n;
		bytes -= (size_t)n;
	}
	return 0;
}

int kvs_transport_read(kvs_transport_t *io, void *data, size_t bytes, unsigned int timeout_ms) {
	return transfer(io, data, bytes, timeout_ms, 0);
}

int kvs_transport_write(kvs_transport_t *io, const void *data, size_t bytes,
                        unsigned int timeout_ms) {
	return transfer(io, (void *)data, bytes, timeout_ms, 1);
}

void kvs_transport_shutdown(kvs_transport_t *io) {
	if (io->fd >= 0)
		(void)IO(io, shutdown, io->fd, SHUT_RDWR);
}

void kvs_transport_close(kvs_transport_t *io) {
	if (io->fd >= 0)
		(void)IO(io, close, io->fd);
	io->fd = -1;
}
