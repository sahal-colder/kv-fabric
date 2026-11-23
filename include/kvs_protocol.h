/*
 * 协议层。统一文本或 RESP 请求的执行入口。
 * 解析只借用输入，回复按上层协议分配。Store 锁用于命令语义，网络层发送回复。
 */
#ifndef KVS_PROTOCOL_H
#define KVS_PROTOCOL_H

#include "kvs_connection.h"
#include "kvs_network.h"

/* Run buffered requests, up to one output batch. 1 means replication owns a
 * duplicate of the socket and the network backend must detach and close fd. */
int kvs_protocol_process(kvs_connection_t *conn);
int kvs_text_command(char *line, char response[BUFFER_LENGTH]);

#endif
