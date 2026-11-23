/*
 * 通用 KVStore 回归层，覆盖 config。
 * 夹具拥有本地数据或连接。业务锁由被测代码管理。
 */
#include "kvs_config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
	kvs_config_t c, before;
	char error[KVS_CONFIG_ERROR_MAX], path[] = "/tmp/kvstore-config-XXXXXX";
	int fd = mkstemp(path);
	assert(fd >= 0);
	FILE *file = fdopen(fd, "w");
	fputs("# configuration\r\nport 21000\nengine hash\nmysql-password \"space # and "
	      "\\\"quote\\\"\"\n",
	      file);
	fclose(file);
	kvs_config_defaults(&c);
	assert(!kvs_config_load(&c, path, error));
	assert(c.port == 21000 && !strcmp(c.engine, "hash"));
	assert(!strcmp(c.mysql_password, "space # and \"quote\""));
	char *args[] = {"kvstore", "--port", "22000", "--config", path, "--persistence", "none"};
	assert(!kvs_config_cli(&c, 7, args, error));
	assert(c.port == 22000 && c.persistence == -1);
	before = c;
	file = fopen(path, "w");
	fputs("port 23000\nunknown-option secret\n", file);
	fclose(file);
	assert(kvs_config_load(&c, path, error) < 0 && !memcmp(&c, &before, sizeof(c)));
	assert(strstr(error, "line 2") && !strstr(error, "secret"));
	char *bad[] = {"kvstore", "--port", "65536"};
	assert(kvs_config_cli(&c, 3, bad, error) < 0 && !memcmp(&c, &before, sizeof(c)));
	char *slave[] = {"kvstore", "--role", "slave"};
	assert(kvs_config_cli(&c, 3, slave, error) < 0);
	file = fopen(path, "w");
	fputs("mysql-password \"unterminated\n", file);
	fclose(file);
	assert(kvs_config_load(&c, path, error) < 0);
	file = tmpfile();
	assert(file);
	kvs_config_print(&c, file);
	rewind(file);
	char output[4096];
	size_t n = fread(output, 1, sizeof(output) - 1, file);
	output[n] = 0;
	assert(strstr(output, "<redacted>") && !strstr(output, "space # and"));
	fclose(file);
	unlink(path);
	puts("config: PASS");
	return 0;
}
