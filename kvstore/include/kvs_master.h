/*
 * 复制层。生成全量同步和有序增量队列。
 * 队列持有待发送数据。先取 Store 锁，再取主节点状态锁，发送在线程中完成。
 */
#ifndef KVS_MASTER_H
#define KVS_MASTER_H

#include <stddef.h>

#include "kvs_mutation.h"

int kvs_master_start(void);
void kvs_master_stop(void);
int kvs_master_register_slave(int fd);
size_t kvs_master_slave_count(void);
void kvs_master_publish(const kvs_write_event_t *event);
void kvs_master_resync(void);

#endif
