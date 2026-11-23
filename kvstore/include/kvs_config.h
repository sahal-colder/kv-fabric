/*
 * 配置边界。解析并校验配置，在启动时发布进程级快照。
 * 调用方拥有临时配置，运行期快照只读。无锁嵌套。
 */
#ifndef KVS_CONFIG_H
#define KVS_CONFIG_H

#include <stddef.h>
#include <stdio.h>
#include "kvs_replication.h"

/* Select exactly one file-storage mode. */
#define KVS_PERSISTENCE_FULL		0
#define KVS_PERSISTENCE_INCREMENTAL	1
#ifndef KVS_PERSISTENCE_MODE
#define KVS_PERSISTENCE_MODE		KVS_PERSISTENCE_FULL
#endif

#if KVS_PERSISTENCE_MODE != KVS_PERSISTENCE_FULL && \
	KVS_PERSISTENCE_MODE != KVS_PERSISTENCE_INCREMENTAL
#error "invalid KVS_PERSISTENCE_MODE"
#endif

#define KVS_PATH_MAX 1024
#define KVS_CONFIG_ERROR_MAX 256

typedef struct kvs_config_s {
	unsigned short port;
	char bind[64];
	char engine[16];
	char network[16];
	int persistence; /* -1: disabled, 0: full snapshot, 1: incremental log. */
	char snapshot_file[KVS_PATH_MAX];
	char aof_file[KVS_PATH_MAX];
	int mysql_enabled;
	char mysql_host[256], mysql_user[128], mysql_password[256];
	char mysql_database[128], mysql_table[128];
	unsigned short mysql_port;
	kvs_replication_config_t replication;
} kvs_config_t;

void kvs_config_defaults(kvs_config_t *config);
/* Parsing is transactional: errors never modify the supplied configuration. */
int kvs_config_load(kvs_config_t *config, const char *path, char *error);
int kvs_config_cli(kvs_config_t *config, int argc, char **argv, char *error);
int kvs_config_validate(const kvs_config_t *config, char *error);
int kvs_config_apply(const kvs_config_t *config, char *error);
const kvs_config_t *kvs_config_get(void);
void kvs_config_print(const kvs_config_t *config, FILE *output);

#endif
