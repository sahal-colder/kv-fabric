/*
 * 复制层。接收、安装全量状态并顺序应用后续变更。
 * 从节点线程拥有复制连接，状态锁只保护连接和停机标志。
 */
#ifndef KVS_SLAVE_H
#define KVS_SLAVE_H

#include "kvs_replication.h"

#define KVS_SLAVE_HANDSHAKE "KVS_SYNC_HELLO_V1\r\n"
#define KVS_MASTER_HANDSHAKE_ACK "KVS_SYNC_OK_V1\r\n"

/* Versioned, checksummed snapshot/update/heartbeat frames follow the handshake. */
int kvs_slave_init(kvs_replication_config_t *config);
void kvs_slave_stop(void);
int kvs_slave_is_connected(void);

#endif
