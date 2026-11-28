/*
 * 通用 KVStore 回归层，覆盖 replication。
 * 夹具拥有本地数据或连接。业务锁由被测代码管理。
 */
#include "kvs_replication.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void expect(int condition, const char *message) {
	if (condition) return;
	fprintf(stderr, "FAIL: %s\n", message);
	failures++;
}

int main(void) {
	kvs_replication_config_t config = {0};
	int role = -1;

	expect(
		kvs_replication_parse_role("standalone", &role) == 0 &&
		role == KVS_ROLE_STANDALONE,
		"parse standalone role"
	);
	expect(
		kvs_replication_parse_role("master", &role) == 0 &&
		role == KVS_ROLE_MASTER,
		"parse master role"
	);
	expect(
		kvs_replication_parse_role("slave", &role) == 0 &&
		role == KVS_ROLE_SLAVE,
		"parse slave role"
	);
	expect(
		kvs_replication_parse_role("invalid", &role) != 0,
		"reject invalid role"
	);

	config.role = KVS_ROLE_STANDALONE;
	expect(
		kvs_replication_configure(&config) == 0,
		"configure standalone role"
	);
	expect(
		strcmp(kvs_replication_role_name(), "standalone") == 0,
		"report standalone role"
	);
	expect(
		kvs_replication_allows_client_writes(),
		"standalone accepts client writes"
	);

	memset(&config, 0, sizeof(config));
	config.role = KVS_ROLE_MASTER;
	strcpy(config.secret,"unit-test-secret-32-bytes-minimum");
	config.listen_port = 2101;
	config.replication_port = 2000;
	expect(
		kvs_replication_configure(&config) == 0,
		"configure master role"
	);
	expect(
		strcmp(kvs_replication_role_name(), "master") == 0,
		"report master role"
	);
	expect(
		kvs_replication_allows_client_writes(),
		"master accepts client writes"
	);

	memset(&config, 0, sizeof(config));
	config.role = KVS_ROLE_SLAVE;
	strcpy(config.secret,"unit-test-secret-32-bytes-minimum");
	memcpy(config.master_host, "127.0.0.1", sizeof("127.0.0.1"));
	config.master_port = 2000;
	expect(
		kvs_replication_configure(&config) == 0,
		"configure slave role"
	);
	expect(
		strcmp(kvs_replication_role_name(), "slave") == 0,
		"report slave role"
	);
	expect(
		!kvs_replication_allows_client_writes(),
		"slave rejects client writes"
	);

	config.master_port = 0;
	expect(
		kvs_replication_configure(&config) != 0,
		"reject slave without master port"
	);
	expect(
		strcmp(kvs_replication_role_name(), "slave") == 0,
		"invalid configuration leaves active role unchanged"
	);

	if (failures != 0) return EXIT_FAILURE;
	printf("replication: PASS\n");
	return EXIT_SUCCESS;
}
