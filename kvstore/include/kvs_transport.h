/*
 * 复制网络层。按配置调用 TCP 或 rsockets。
 * 调用方持有连接和收发缓冲区。无内置 Store 锁。
 */
#ifndef KVS_TRANSPORT_H
#define KVS_TRANSPORT_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
	int fd;
	int rdma;
} kvs_transport_t;

int kvs_transport_connect(kvs_transport_t *io, const char *host, unsigned short port, int rdma,
                          unsigned int timeout_ms);
int kvs_transport_listen(kvs_transport_t *io, const char *host, unsigned short port, int rdma);
int kvs_transport_accept(kvs_transport_t *listener, kvs_transport_t *peer, unsigned int timeout_ms);
int kvs_transport_wait(kvs_transport_t *io, short events, unsigned int timeout_ms);
int kvs_transport_read(kvs_transport_t *io, void *data, size_t bytes, unsigned int timeout_ms);
int kvs_transport_write(kvs_transport_t *io, const void *data, size_t bytes,
                        unsigned int timeout_ms);
void kvs_transport_shutdown(kvs_transport_t *io);
void kvs_transport_close(kvs_transport_t *io);

/* Only replication sockets are enrolled; normal client traffic is unaffected. */
int kvs_bpf_start(void);
void kvs_bpf_stop(void);
int kvs_bpf_add(int fd);
void kvs_bpf_remove(int fd);

#endif
