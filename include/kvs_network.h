/*
 * 网络层的 Reactor、Proactor 和 NtyCo 后端入口。
 * 事件循环接管监听套接字，应用命令经共享 Store 锁。
 */
#ifndef KVS_NETWORK_H
#define KVS_NETWORK_H

#define BUFFER_LENGTH 1024

int kvs_tcp_listen(unsigned short port, int flags);
int kvs_tcp_nodelay(int fd);
int reactor_start_protocol(unsigned short port);
int proactor_start_protocol(unsigned short port);
int ntyco_start_protocol(unsigned short port);

#endif
