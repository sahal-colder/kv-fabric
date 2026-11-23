/*
 * 复制控制层。定义角色、配置和发布事件的取值约束。
 * 事件字符串由上层或日志队列持有。配置阶段无并发访问。
 */
#ifndef KVS_REPLICATION_H
#define KVS_REPLICATION_H

#include <stddef.h>

#define KVS_ROLE_STANDALONE 0
#define KVS_ROLE_MASTER     1
#define KVS_ROLE_SLAVE      2

#ifndef KVS_DEFAULT_ROLE
#define KVS_DEFAULT_ROLE KVS_ROLE_STANDALONE
#endif

#define KVS_REPLICATION_MAX_COMMAND_SIZE (1024U * 1024U)

enum { KVS_SYNC_TCP = 0, KVS_SYNC_RDMA = 1 };

typedef struct kvs_replication_config_s {
	int role;
	unsigned short replication_port;
	char master_host[256];
	unsigned short master_port;
	int transport;
	char bind_host[64];
	unsigned short listen_port; /* 0: use the client listener (TCP only). */
	unsigned int timeout_ms;
	unsigned int reconnect_ms;
	size_t queue_bytes;
	size_t snapshot_bytes;
	int ebpf_enabled;
	char ebpf_object[1024];
	char ebpf_map[1024];
} kvs_replication_config_t;

/* Apply a complete startup configuration before replication threads start. */
int kvs_replication_configure(const kvs_replication_config_t *config);
const kvs_replication_config_t *kvs_replication_config(void);
const char *kvs_replication_role_name(void);
int kvs_replication_parse_role(const char *name, int *role);
int kvs_replication_allows_client_writes(void);

#endif
