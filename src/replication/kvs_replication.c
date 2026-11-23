/*
 * 复制控制层。定义角色、配置和发布事件的取值约束。
 * 事件字符串由上层或日志队列持有。配置阶段无并发访问。
 */
#include "kvs_replication.h"

#include <string.h>

static kvs_replication_config_t kvs_config = {
	.role = KVS_DEFAULT_ROLE,
	.master_host = "127.0.0.1"
};

int kvs_replication_configure(const kvs_replication_config_t *config) {
	if (config == NULL ||
		(config->role != KVS_ROLE_STANDALONE &&
		 config->role != KVS_ROLE_MASTER &&
		 config->role != KVS_ROLE_SLAVE)) {
		return -1;
	}
	if (config->role == KVS_ROLE_MASTER &&
		config->replication_port == 0) return -1;
	if (config->role == KVS_ROLE_SLAVE &&
		(config->master_host[0] == '\0' || config->master_port == 0)) {
		return -1;
	}
	kvs_config = *config;
	kvs_config.master_host[sizeof(kvs_config.master_host) - 1] = '\0';
	return 0;
}

const kvs_replication_config_t *kvs_replication_config(void) {
	return &kvs_config;
}

const char *kvs_replication_role_name(void) {
	switch (kvs_config.role) {
	case KVS_ROLE_MASTER:
		return "master";
	case KVS_ROLE_SLAVE:
		return "slave";
	default:
		return "standalone";
	}
}

int kvs_replication_parse_role(const char *name, int *role) {
	if (name == NULL || role == NULL) return -1;
	if (strcmp(name, "standalone") == 0) {
		*role = KVS_ROLE_STANDALONE;
		return 0;
	}
	if (strcmp(name, "master") == 0) {
		*role = KVS_ROLE_MASTER;
		return 0;
	}
	if (strcmp(name, "slave") == 0) {
		*role = KVS_ROLE_SLAVE;
		return 0;
	}
	return -1;
}

int kvs_replication_allows_client_writes(void) {
	return kvs_config.role != KVS_ROLE_SLAVE;
}
