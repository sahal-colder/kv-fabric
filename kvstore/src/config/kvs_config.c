/*
 * 配置边界。解析并校验配置，在启动时发布进程级快照。
 * 调用方拥有临时配置，运行期快照只读。无锁嵌套。
 */
#include "kvs_config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

static kvs_config_t active;
static int initialized;

static int fail(char *error, const char *format, ...) {
	va_list args;
	va_start(args, format);
	if (error)
		vsnprintf(error, KVS_CONFIG_ERROR_MAX, format, args);
	va_end(args);
	return -1;
}

void kvs_config_defaults(kvs_config_t *c) {
	memset(c, 0, sizeof(*c));
	c->port = 2000;
	strcpy(c->bind, "127.0.0.1");
	strcpy(c->engine, "array");
	strcpy(c->network, "reactor");
	c->persistence = KVS_PERSISTENCE_MODE;
	strcpy(c->snapshot_file, "data/kvstore.snapshot");
	strcpy(c->aof_file, "data/kvstore.aof");
	strcpy(c->mysql_host, "127.0.0.1");
	strcpy(c->mysql_user, "root");
	strcpy(c->mysql_database, "kvstore");
	strcpy(c->mysql_table, "kvs_records");
	c->mysql_port = 3306;
	c->replication.role = KVS_DEFAULT_ROLE;
	c->replication.timeout_ms = 5000;
	c->replication.reconnect_ms = 1000;
	c->replication.queue_bytes = 8U * 1024U * 1024U;
	c->replication.snapshot_bytes = 64U * 1024U * 1024U;
	strcpy(c->replication.ebpf_object, "build/bpf/kvs_sockhash.bpf.o");
	strcpy(c->replication.ebpf_map, "/sys/fs/bpf/kvstore_sync");
}

static int number(const char *text, size_t max, size_t *value) {
	char *end;
	unsigned long long n;
	if (!text[0] || !isdigit((unsigned char)text[0]))
		return -1;
	errno = 0;
	n = strtoull(text, &end, 10);
	if (errno || *end || n > max)
		return -1;
	*value = (size_t)n;
	return 0;
}

static int set(kvs_config_t *c, const char *key, const char *value, char *error) {
	size_t n;
#define TEXT(name, field)                                                                          \
	if (!strcmp(key, name)) {                                                                      \
		if (strlen(value) >= sizeof(c->field))                                                     \
			return fail(error, "%s is too long", key);                                             \
		strcpy(c->field, value);                                                                   \
		return 0;                                                                                  \
	}
#define UINT(name, field, maximum)                                                                 \
	if (!strcmp(key, name)) {                                                                      \
		if (number(value, maximum, &n))                                                            \
			return fail(error, "invalid %s", key);                                                 \
		c->field = n;                                                                              \
		return 0;                                                                                  \
	}
#define BOOL(name, field)                                                                          \
	if (!strcmp(key, name)) {                                                                      \
		if (strcmp(value, "yes") && strcmp(value, "no"))                                           \
			return fail(error, "%s expects yes or no", key);                                       \
		c->field = !strcmp(value, "yes");                                                          \
		return 0;                                                                                  \
	}
	TEXT("bind", bind)
	TEXT("engine", engine)
	TEXT("network", network)
	TEXT("snapshot-file", snapshot_file)
	TEXT("aof-file", aof_file)
	TEXT("master-host", replication.master_host)
	TEXT("mysql-host", mysql_host)
	TEXT("mysql-user", mysql_user)
	TEXT("mysql-password", mysql_password)
	TEXT("mysql-database", mysql_database)
	TEXT("mysql-table", mysql_table)
	TEXT("ebpf-object", replication.ebpf_object)
	TEXT("ebpf-map", replication.ebpf_map)
	UINT("port", port, 65535)
	UINT("master-port", replication.master_port, 65535)
	UINT("replication-port", replication.listen_port, 65535)
	UINT("sync-timeout-ms", replication.timeout_ms, 600000)
	UINT("sync-reconnect-ms", replication.reconnect_ms, 600000)
	UINT("sync-queue-bytes", replication.queue_bytes, 1024U * 1024U * 1024U)
	UINT("sync-snapshot-bytes", replication.snapshot_bytes, 1024U * 1024U * 1024U)
	UINT("mysql-port", mysql_port, 65535)
	BOOL("mysql-enabled", mysql_enabled)
	BOOL("ebpf-enabled", replication.ebpf_enabled)
#undef TEXT
#undef UINT
#undef BOOL
	if (!strcmp(key, "role")) {
		if (!kvs_replication_parse_role(value, &c->replication.role))
			return 0;
	} else if (!strcmp(key, "sync-transport")) {
		if (!strcmp(value, "tcp")) {
			c->replication.transport = KVS_SYNC_TCP;
			return 0;
		}
		if (!strcmp(value, "rdma")) {
			c->replication.transport = KVS_SYNC_RDMA;
			return 0;
		}
	} else if (!strcmp(key, "persistence")) {
		if (!strcmp(value, "none")) {
			c->persistence = -1;
			return 0;
		}
		if (!strcmp(value, "full")) {
			c->persistence = 0;
			return 0;
		}
		if (!strcmp(value, "aof")) {
			c->persistence = 1;
			return 0;
		}
	} else
		return fail(error, "unknown directive: %s", key);
	return fail(error, "invalid %s", key);
}

/* Redis-style words, quotes, escapes, and comments; no expansion or execution. */
static int word(char **cursor, char *out, size_t size) {
	char *p = *cursor, quote = 0;
	size_t used = 0;
	while (isspace((unsigned char)*p))
		p++;
	if (!*p || *p == '#') {
		*cursor = p;
		return 0;
	}
	if (*p == '\'' || *p == '"')
		quote = *p++;
	while (*p && (quote ? *p != quote : !isspace((unsigned char)*p) && *p != '#')) {
		char ch = *p++;
		if (ch == '\\') {
			if (!*p)
				return -1;
			ch = *p++;
			if (ch == 'n')
				ch = '\n';
			else if (ch == 'r')
				ch = '\r';
			else if (ch == 't')
				ch = '\t';
			else if (ch != '\\' && ch != '\'' && ch != '"' && ch != '#' && ch != ' ')
				return -1;
		}
		if (used + 1 >= size)
			return -1;
		out[used++] = ch;
	}
	if (quote && *p++ != quote)
		return -1;
	if (quote && *p && !isspace((unsigned char)*p) && *p != '#')
		return -1;
	out[used] = 0;
	*cursor = p;
	return 1;
}

int kvs_config_load(kvs_config_t *config, const char *path, char *error) {
	kvs_config_t next = *config;
	FILE *file = fopen(path, "r");
	char line[4096], key[64], value[KVS_PATH_MAX], extra[2], detail[KVS_CONFIG_ERROR_MAX];
	unsigned int index = 0;
	if (!file)
		return fail(error, "cannot open config: %s", path);
	while (fgets(line, sizeof(line), file)) {
		char *p = line;
		int found;
		index++;
		if (!strchr(line, '\n') && !feof(file))
			goto invalid;
		found = word(&p, key, sizeof(key));
		if (!found)
			continue;
		if (found < 0 || word(&p, value, sizeof(value)) != 1 || word(&p, extra, sizeof(extra)) != 0)
			goto invalid;
		if (set(&next, key, value, detail)) {
			fclose(file);
			return fail(error, "line %u: %s", index, detail);
		}
	}
	if (ferror(file)) {
		fclose(file);
		return fail(error, "cannot read config");
	}
	fclose(file);
	*config = next;
	return 0;
invalid:
	fclose(file);
	return fail(error, "line %u: expected directive and one value", index);
}

int kvs_config_cli(kvs_config_t *config, int argc, char **argv, char *error) {
	kvs_config_t next = *config;
	int index = 1;
	/* Load the file first, then apply CLI overrides regardless of their position. */
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--config")) {
			if (++i == argc)
				return fail(error, "missing --config path");
			if (kvs_config_load(&next, argv[i], error))
				return -1;
		}
	}
	if (index < argc && argv[index][0] != '-') {
		if (set(&next, "port", argv[index++], error))
			return -1;
		if (index < argc && argv[index][0] != '-') {
			if (set(&next, "engine", argv[index++], error))
				return -1;
		}
	}
	while (index < argc) {
		const char *arg = argv[index++];
		if (!strcmp(arg, "--check-config") || !strcmp(arg, "--print-config"))
			continue;
		if (!strcmp(arg, "--config")) {
			if (index == argc)
				return fail(error, "missing config path");
			index++;
			continue;
		}
		if (strncmp(arg, "--", 2) || index == argc)
			return fail(error, "expected --directive value");
		if (set(&next, arg + 2, argv[index++], error))
			return -1;
	}
	if (kvs_config_validate(&next, error))
		return -1;
	*config = next;
	return 0;
}

int kvs_config_validate(const kvs_config_t *c, char *error) {
	struct in_addr addr;
	const kvs_replication_config_t *r = &c->replication;
	if (!c->port || inet_pton(AF_INET, c->bind, &addr) != 1)
		return fail(error, "port and IPv4 bind address are required");
	if (strcmp(c->engine, "array") && strcmp(c->engine, "rbtree") && strcmp(c->engine, "hash") &&
	    strcmp(c->engine, "skiplist"))
		return fail(error, "invalid engine");
	if (strcmp(c->network, "reactor") && strcmp(c->network, "proactor") &&
	    strcmp(c->network, "ntyco"))
		return fail(error, "invalid network");
#if !KVS_ENABLE_PROACTOR
	if (!strcmp(c->network, "proactor"))
		return fail(error, "proactor requires make PROACTOR=1");
#endif
#if !KVS_ENABLE_NTYCO
	if (!strcmp(c->network, "ntyco"))
		return fail(error, "ntyco requires make NTYCO=1");
#endif
	if (c->persistence < -1 || c->persistence > 1 || !c->snapshot_file[0] || !c->aof_file[0])
		return fail(error, "invalid persistence configuration");
	if (r->role < KVS_ROLE_STANDALONE || r->role > KVS_ROLE_SLAVE)
		return fail(error, "invalid role");
	if (r->role == KVS_ROLE_SLAVE && (!r->master_host[0] || !r->master_port))
		return fail(error, "slave requires master-host and master-port");
	if (r->timeout_ms < 2000)
		return fail(error, "sync-timeout-ms must be at least 2000 for heartbeats");
	if (r->role != KVS_ROLE_SLAVE && (r->master_host[0] || r->master_port))
		return fail(error, "master endpoint requires slave role");
	if (r->transport != KVS_SYNC_TCP && r->transport != KVS_SYNC_RDMA)
		return fail(error, "invalid sync transport");
	if (r->transport == KVS_SYNC_RDMA && r->role == KVS_ROLE_MASTER &&
	    (!r->listen_port || r->listen_port == c->port))
		return fail(error, "RDMA master requires a separate replication-port");
	if (r->listen_port && r->listen_port == c->port)
		return fail(error, "replication-port must differ from port; use 0 to share TCP listener");
	if (!r->reconnect_ms || r->queue_bytes < 4096 || r->snapshot_bytes < 4096)
		return fail(error, "sync budgets and timeouts must be positive (budgets >=4096)");
	if (r->ebpf_enabled &&
	    (r->transport != KVS_SYNC_TCP || !r->ebpf_object[0] || r->ebpf_map[0] != '/'))
		return fail(error, "eBPF requires TCP, ebpf-object and an absolute bpffs map path");
#if !KVS_ENABLE_RDMA
	if (r->transport == KVS_SYNC_RDMA)
		return fail(error, "RDMA requires make RDMA=1");
#endif
#if !KVS_ENABLE_BPF
	if (r->ebpf_enabled)
		return fail(error, "eBPF requires make BPF=1");
#endif
#if !ENABLE_MYSQL
	if (c->mysql_enabled)
		return fail(error, "MySQL requires make MYSQL=1");
#endif
	if (c->mysql_enabled && (!c->mysql_port || !c->mysql_host[0] || !c->mysql_user[0]))
		return fail(error, "invalid MySQL endpoint");
	return 0;
}

int kvs_config_apply(const kvs_config_t *config, char *error) {
	if (kvs_config_validate(config, error))
		return -1;
	active = *config;
	active.replication.replication_port = active.port;
	strcpy(active.replication.bind_host, active.bind);
	if (kvs_replication_configure(&active.replication))
		return fail(error, "invalid replication configuration");
	initialized = 1;
	return 0;
}

const kvs_config_t *kvs_config_get(void) {
	if (!initialized) {
		kvs_config_defaults(&active);
		initialized = 1;
	}
	return &active;
}

void kvs_config_print(const kvs_config_t *c, FILE *out) {
	fprintf(
	    out,
	    "bind %s\nport %u\nengine %s\nnetwork %s\npersistence %s\n"
	    "role %s\nsync-transport %s\nreplication-port %u\nmaster-host %s\nmaster-port %u\n"
	    "sync-timeout-ms %u\nsync-reconnect-ms %u\nsync-queue-bytes %zu\nsync-snapshot-bytes %zu\n"
	    "ebpf-enabled %s\nmysql-enabled %s\nmysql-password <redacted>\n",
	    c->bind, c->port, c->engine, c->network,
	    c->persistence < 0 ? "none"
		: c->persistence   ? "aof"
		                   : "full",
	    c->replication.role == 1   ? "master"
		: c->replication.role == 2 ? "slave"
		                           : "standalone",
	    c->replication.transport ? "rdma" : "tcp", c->replication.listen_port,
	    c->replication.master_host, c->replication.master_port, c->replication.timeout_ms,
	    c->replication.reconnect_ms, c->replication.queue_bytes, c->replication.snapshot_bytes,
	    c->replication.ebpf_enabled ? "yes" : "no", c->mysql_enabled ? "yes" : "no");
}
