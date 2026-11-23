/*
 * 命令层。将 Redis 兼容命令映射到通用 Store。
 * 请求与回复归连接。先取 Store 锁，再进入引擎或复制元数据。
 */
#include "kvs_redis.h"
#include "kvs_engine.h"
#include "kvs_allocator.h"
#include "kvs_master.h"
#include "kvs_replication.h"
#include "kvs_slave.h"
#include "kvs_persistence.h"
#include "kvs_config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int kvs_resp_wrong_args(char *response,size_t *used,const char *command_name) {
	return kvs_resp_append(
		response,
		used,
	 "-ERR wrong number of arguments for '%s' command\r\n",
		command_name
	);
}

static int kvs_resp_change_integer(
	const char *key,
	long long delta,
	char *response,
	size_t *used
) {
	char *end = NULL;
	char number[64];
	const char *value = kvs_resp_engine_get(key);
	long long current;

	if (value == NULL) {
		current = 0;
	} else {
		errno = 0;
		current = strtoll(value, &end, 10);
		if (errno == ERANGE || end == value || *end != '\0') {
			return kvs_resp_append(
				response,
				used,
				"-ERR value is not an integer or out of range\r\n"
			);
		}
	}
	if ((delta > 0 && current > LLONG_MAX - delta) ||
		(delta < 0 && current < LLONG_MIN - delta)) {
		return kvs_resp_append(response, used, "-ERR increment or decrement would overflow\r\n");
	}
	current += delta;
	snprintf(number, sizeof(number), "%lld", current);
	if (kvs_resp_set_string(key, number) < 0) return -1;
	return kvs_resp_append(response, used, ":%s\r\n", number);
}

static int kvs_resp_command_lock_mode(kvs_resp_arg_t *args) {
	if (kvs_resp_arg_equals(&args[0], "GET") ||
		kvs_resp_arg_equals(&args[0], "MGET") ||
		kvs_resp_arg_equals(&args[0], "EXISTS") ||
		kvs_resp_arg_equals(&args[0], "TOUCH") ||
		kvs_resp_arg_equals(&args[0], "STRLEN") ||
		kvs_resp_arg_equals(&args[0], "TYPE") ||
		kvs_resp_arg_equals(&args[0], "DBSIZE")) {
		return 1;
	}

	if (kvs_resp_arg_equals(&args[0], "SET") ||
		kvs_resp_arg_equals(&args[0], "SETNX") ||
		kvs_resp_arg_equals(&args[0], "GETSET") ||
		kvs_resp_arg_equals(&args[0], "GETDEL") ||
		kvs_resp_arg_equals(&args[0], "APPEND") ||
		kvs_resp_arg_equals(&args[0], "RENAME") ||
		kvs_resp_arg_equals(&args[0], "RENAMENX") ||
		kvs_resp_arg_equals(&args[0], "DEL") ||
		kvs_resp_arg_equals(&args[0], "UNLINK") ||
		kvs_resp_arg_equals(&args[0], "MSET") ||
		kvs_resp_arg_equals(&args[0], "MSETNX") ||
		kvs_resp_arg_equals(&args[0], "INCR") ||
		kvs_resp_arg_equals(&args[0], "DECR") ||
		kvs_resp_arg_equals(&args[0], "INCRBY") ||
		kvs_resp_arg_equals(&args[0], "DECRBY") ||
		kvs_resp_arg_equals(&args[0], "FLUSHDB") ||
		kvs_resp_arg_equals(&args[0], "FLUSHALL")) {
		return 2;
	}

	return 0;
}

static int kvs_resp_execute_command_unlocked(kvs_resp_arg_t *args,int argc,char *response,size_t *used) {
	const char *key;
	const char *value;

	if (kvs_resp_arg_equals(&args[0], "HELLO")) {
		const char *role = kvs_replication_role_name();
		if (argc > 2) return kvs_resp_wrong_args(response, used, "hello");
		if (argc == 2 &&
			(args[1].length != 1 || args[1].data[0] != '2')) {
			return kvs_resp_append(
				response,
				used,
				"-NOPROTO unsupported protocol version\r\n"
			);
		}
		if (kvs_resp_append(response, used, "*14\r\n") != 0 ||
			kvs_resp_append_bulk(response, used, "server", 6) != 0 ||
			kvs_resp_append_bulk(response, used, "kvstore", 7) != 0 ||
			kvs_resp_append_bulk(response, used, "version", 7) != 0 ||
			kvs_resp_append_bulk(response, used, "0.1.0", 5) != 0 ||
			kvs_resp_append_bulk(response, used, "proto", 5) != 0 ||
			kvs_resp_append(response, used, ":2\r\n") != 0 ||
			kvs_resp_append_bulk(response, used, "id", 2) != 0 ||
			kvs_resp_append(response, used, ":1\r\n") != 0 ||
			kvs_resp_append_bulk(response, used, "mode", 4) != 0 ||
			kvs_resp_append_bulk(response, used, "standalone", 10) != 0 ||
			kvs_resp_append_bulk(response, used, "role", 4) != 0 ||
			kvs_resp_append_bulk(response, used, role, strlen(role)) != 0 ||
			kvs_resp_append_bulk(response, used, "modules", 7) != 0 ||
			kvs_resp_append(response, used, "*0\r\n") != 0) {
			return -1;
		}
		return 0;
	}

	if (kvs_resp_arg_equals(&args[0], "PING")) {
		if (argc == 1) return kvs_resp_append(response, used, "+PONG\r\n");
		if (argc == 2) {
			return kvs_resp_append_bulk(response, used, args[1].data, args[1].length);
		}
		return kvs_resp_wrong_args(response, used, "ping");
	}

	if (kvs_resp_arg_equals(&args[0], "ECHO")) {
		if (argc != 2) return kvs_resp_wrong_args(response, used, "echo");
		return kvs_resp_append_bulk(response, used, args[1].data, args[1].length);
	}

	if (kvs_resp_arg_equals(&args[0], "SET")) {
		int set_nx = 0;
		int set_xx = 0;
		int return_old = 0;
		const char *old_value;

		if (argc < 3) return kvs_resp_wrong_args(response, used, "set");
		for (int i = 3; i < argc; i++) {
			if (kvs_resp_arg_equals(&args[i], "NX")) {
				if (set_nx) {
					return kvs_resp_append(response, used, "-ERR syntax error\r\n");
				}
				set_nx = 1;
			} else if (kvs_resp_arg_equals(&args[i], "XX")) {
				if (set_xx) {
					return kvs_resp_append(response, used, "-ERR syntax error\r\n");
				}
				set_xx = 1;
			} else if (kvs_resp_arg_equals(&args[i], "GET")) {
				if (return_old) {
					return kvs_resp_append(response, used, "-ERR syntax error\r\n");
				}
				return_old = 1;
			} else {
				return kvs_resp_append(
					response,
					used,
					"-ERR syntax error or unsupported SET option\r\n"
				);
			}
		}
		if (set_nx && set_xx) {
			return kvs_resp_append(response, used, "-ERR syntax error\r\n");
		}

		key = args[1].data;
		value = args[2].data;
		old_value = kvs_resp_engine_get(key);
		if ((set_nx && old_value != NULL) ||
			(set_xx && old_value == NULL)) {
			return kvs_resp_append(response, used, "$-1\r\n");
		}
		size_t response_start = *used;
		if (return_old &&
			kvs_resp_append_value(response, used, old_value) != 0) {
			return -1;
		}
		if (kvs_resp_set_string(key, value) < 0) {
			*used = response_start;
			return kvs_resp_append(response, used, "-ERR set failed\r\n");
		}
		if (return_old) return 0;
		return kvs_resp_append(response, used, "+OK\r\n");
	}

	if (kvs_resp_arg_equals(&args[0], "SETNX")) {
		if (argc != 3) return kvs_resp_wrong_args(response, used, "setnx");
		if (kvs_resp_engine_get(args[1].data) != NULL) {
			return kvs_resp_append(response, used, ":0\r\n");
		}
		if (kvs_resp_set_string(args[1].data, args[2].data) < 0) {
			return kvs_resp_append(response, used, "-ERR set failed\r\n");
		}
		return kvs_resp_append(response, used, ":1\r\n");
	}

	if (kvs_resp_arg_equals(&args[0], "GET")) {
		if (argc != 2) return kvs_resp_wrong_args(response, used, "get");
		return kvs_resp_append_value(
			response,
			used,
			kvs_resp_engine_get(args[1].data)
		);
	}

	if (kvs_resp_arg_equals(&args[0], "GETSET")) {
		const char *old_value;
		if (argc != 3) return kvs_resp_wrong_args(response, used, "getset");
		old_value = kvs_resp_engine_get(args[1].data);
		if (kvs_resp_append_value(response, used, old_value) != 0) return -1;
		if (kvs_resp_set_string(args[1].data, args[2].data) < 0) return -1;
		return 0;
	}

	if (kvs_resp_arg_equals(&args[0], "GETDEL")) {
		const char *old_value;
		if (argc != 2) return kvs_resp_wrong_args(response, used, "getdel");
		old_value = kvs_resp_engine_get(args[1].data);
		if (kvs_resp_append_value(response, used, old_value) != 0) return -1;
		if (old_value != NULL && kvs_resp_engine_delete(args[1].data) < 0) {
			return -1;
		}
		return 0;
	}

	if (kvs_resp_arg_equals(&args[0], "APPEND")) {
		const char *old_value;
		char *combined;
		size_t old_length;
		size_t new_length;
		int result;

		if (argc != 3) return kvs_resp_wrong_args(response, used, "append");
		old_value = kvs_resp_engine_get(args[1].data);
		old_length = old_value == NULL ? 0 : strlen(old_value);
		new_length = old_length + args[2].length;
		if (new_length < old_length || new_length > KVS_MAX_FIELD_BYTES) {
			return kvs_resp_append(response, used, "-ERR string exceeds maximum size\r\n");
		}
		combined = kvs_malloc(new_length + 1);
		if (combined == NULL) return -1;
		if (old_length > 0) memcpy(combined, old_value, old_length);
		memcpy(combined + old_length, args[2].data, args[2].length);
		combined[new_length] = '\0';
		result = kvs_resp_set_string(args[1].data, combined);
		kvs_free(combined);
		if (result < 0) return -1;
		return kvs_resp_append(response, used, ":%zu\r\n", new_length);
	}

	if (kvs_resp_arg_equals(&args[0], "RENAME") ||
		kvs_resp_arg_equals(&args[0], "RENAMENX")) {
		const char *source_value;
		char *value_copy;
		size_t value_length;
		int rename_nx = kvs_resp_arg_equals(&args[0], "RENAMENX");

		if (argc != 3) {
			return kvs_resp_wrong_args(
				response,
				used,
				rename_nx ? "renamenx" : "rename"
			);
		}
		source_value = kvs_resp_engine_get(args[1].data);
		if (source_value == NULL) {
			return kvs_resp_append(response, used, "-ERR no such key\r\n");
		}
		if (strcmp(args[1].data, args[2].data) == 0) {
			return rename_nx ?
				kvs_resp_append(response, used, ":0\r\n") :
				kvs_resp_append(response, used, "+OK\r\n");
		}
		if (rename_nx && kvs_resp_engine_get(args[2].data) != NULL) {
			return kvs_resp_append(response, used, ":0\r\n");
		}

		value_length = strlen(source_value);
		value_copy = kvs_malloc(value_length + 1);
		if (value_copy == NULL) return -1;
		memcpy(value_copy, source_value, value_length + 1);
		if (kvs_resp_set_string(args[2].data, value_copy) < 0 ||
			kvs_resp_engine_delete(args[1].data) < 0) {
			kvs_free(value_copy);
			return -1;
		}
		kvs_free(value_copy);
		return rename_nx ?
			kvs_resp_append(response, used, ":1\r\n") :
			kvs_resp_append(response, used, "+OK\r\n");
	}

	if (kvs_resp_arg_equals(&args[0], "DEL") ||
		kvs_resp_arg_equals(&args[0], "UNLINK") ||
		kvs_resp_arg_equals(&args[0], "EXISTS") ||
		kvs_resp_arg_equals(&args[0], "TOUCH")) {
		int result = 0;
		int only_count = kvs_resp_arg_equals(&args[0], "EXISTS") ||
			kvs_resp_arg_equals(&args[0], "TOUCH");
		if (argc < 2) {
			return kvs_resp_wrong_args(
				response,
				used,
				kvs_resp_arg_equals(&args[0], "UNLINK") ? "unlink" :
				kvs_resp_arg_equals(&args[0], "TOUCH") ? "touch" :
				only_count ? "exists" : "del"
			);
		}
		for (int i = 1; i < argc; i++) {
			if (kvs_resp_engine_get(args[i].data) == NULL) {
				continue;
			}
			result++;
			if (!only_count && kvs_resp_engine_delete(args[i].data) < 0) {
				return kvs_resp_append(response, used, "-ERR del failed\r\n");
			}
		}
		return kvs_resp_append(response, used, ":%d\r\n", result);
	}

	if (kvs_resp_arg_equals(&args[0], "MSET")) {
		if (argc < 3 || (argc - 1) % 2 != 0) {
			return kvs_resp_wrong_args(response, used, "mset");
		}
		for (int i = 1; i < argc; i += 2) {
			if (kvs_resp_set_string(args[i].data, args[i + 1].data) < 0) {
				return kvs_resp_append(response, used, "-ERR mset failed\r\n");
			}
		}
		return kvs_resp_append(response, used, "+OK\r\n");
	}

	if (kvs_resp_arg_equals(&args[0], "MSETNX")) {
		if (argc < 3 || (argc - 1) % 2 != 0) {
			return kvs_resp_wrong_args(response, used, "msetnx");
		}
		for (int i = 1; i < argc; i += 2) {
			if (kvs_resp_engine_get(args[i].data) != NULL) {
				return kvs_resp_append(response, used, ":0\r\n");
			}
		}
		for (int i = 1; i < argc; i += 2) {
			if (kvs_resp_set_string(args[i].data, args[i + 1].data) < 0) {
				return -1;
			}
		}
		return kvs_resp_append(response, used, ":1\r\n");
	}

	if (kvs_resp_arg_equals(&args[0], "MGET")) {
		if (argc < 2) return kvs_resp_wrong_args(response, used, "mget");
		if (kvs_resp_append(response, used, "*%d\r\n", argc - 1) != 0) {
			return -1;
		}
		for (int i = 1; i < argc; i++) {
			if (kvs_resp_append_value(response,used,
				kvs_resp_engine_get(args[i].data)) != 0) {
				return -1;
			}
		}
		return 0;
	}

	if (kvs_resp_arg_equals(&args[0], "INCR") ||
		kvs_resp_arg_equals(&args[0], "DECR")) {
		int is_decr = kvs_resp_arg_equals(&args[0], "DECR");
		if (argc != 2) {
			return kvs_resp_wrong_args(
				response,
				used,
				is_decr ? "decr" : "incr"
			);
		}
		return kvs_resp_change_integer(
			args[1].data,
			is_decr ? -1 : 1,
			response,
			used
		);
	}

	if (kvs_resp_arg_equals(&args[0], "INCRBY") ||
		kvs_resp_arg_equals(&args[0], "DECRBY")) {
		char *end = NULL;
		long long amount;
		long long delta;
		int is_decr = kvs_resp_arg_equals(&args[0], "DECRBY");

		if (argc != 3) {
			return kvs_resp_wrong_args(
				response,
				used,
				is_decr ? "decrby" : "incrby"
			);
		}
		errno = 0;
		amount = strtoll(args[2].data, &end, 10);
		if (errno == ERANGE || end == args[2].data || *end != '\0') {
			return kvs_resp_append(
				response,
				used,
				"-ERR value is not an integer or out of range\r\n"
			);
		}
		if (is_decr && amount == LLONG_MIN) {
			return kvs_resp_append(
				response,
				used,
				"-ERR increment or decrement would overflow\r\n"
			);
		}
		delta = is_decr ? -amount : amount;
		return kvs_resp_change_integer(
			args[1].data,
			delta,
			response,
			used
		);
	}

	if (kvs_resp_arg_equals(&args[0], "STRLEN")) {
		if (argc != 2) return kvs_resp_wrong_args(response, used, "strlen");
		value = kvs_resp_engine_get(args[1].data);
		return kvs_resp_append(response, used, ":%zu\r\n",
			value == NULL ? 0U : strlen(value));
	}

	if (kvs_resp_arg_equals(&args[0], "TYPE")) {
		if (argc != 2) return kvs_resp_wrong_args(response, used, "type");
		return kvs_resp_append(
			response,
			used,
			kvs_resp_engine_get(args[1].data) == NULL ?
				"+none\r\n" : "+string\r\n"
		);
	}

	if (kvs_resp_arg_equals(&args[0], "DBSIZE")) {
		if (argc != 1) return kvs_resp_wrong_args(response, used, "dbsize");
		return kvs_resp_append(response, used, ":%d\r\n", kvs_resp_key_count());
	}

	if (kvs_resp_arg_equals(&args[0], "FLUSHDB") ||
		kvs_resp_arg_equals(&args[0], "FLUSHALL")) {
		if (argc != 1) {
			return kvs_resp_wrong_args(
				response,
				used,
				kvs_resp_arg_equals(&args[0], "FLUSHALL") ? "flushall" : "flushdb"
			);
		}
		if (kvs_resp_engine_clear() != 0) {
			return kvs_resp_append(response, used, "-ERR flush failed\r\n");
		}
		return kvs_resp_append(response, used, "+OK\r\n");
	}

	if (kvs_resp_arg_equals(&args[0], "SAVE")) {
		const char *filename;
		if (argc != 1 && (argc != 2 || kvs_config_get()->persistence != KVS_PERSISTENCE_FULL))
			return kvs_resp_wrong_args(response, used, "save");
		filename = argc == 2 ? args[1].data : NULL;
		if (kvs_persistence_save(filename) != 0) {
			return kvs_resp_append(response, used, "-ERR save failed\r\n");
		}
		return kvs_resp_append(response, used, "+OK\r\n");
	}

	if (kvs_resp_arg_equals(&args[0], "LOAD")) {
		const char *filename;
		if (argc != 1 && (argc != 2 || kvs_config_get()->persistence != KVS_PERSISTENCE_FULL))
			return kvs_resp_wrong_args(response, used, "load");
		filename = argc == 2 ? args[1].data : NULL;
		if (kvs_engine_load(filename) != 0) {
			return kvs_resp_append(response, used, "-ERR load failed\r\n");
		}
		return kvs_resp_append(response, used, "+OK\r\n");
	}

	if (kvs_resp_arg_equals(&args[0], "SELECT")) {
		if (argc != 2) return kvs_resp_wrong_args(response, used, "select");
		if (args[1].length != 1 || args[1].data[0] != '0') {
			return kvs_resp_append(response, used, "-ERR DB index is out of range\r\n");
		}
		return kvs_resp_append(response, used, "+OK\r\n");
	}

	if (kvs_resp_arg_equals(&args[0], "INFO")) {
		const kvs_replication_config_t *replication =
			kvs_replication_config();
		char info[768];
		int info_length;
		if (argc > 2) return kvs_resp_wrong_args(response, used, "info");
		if (replication->role == KVS_ROLE_SLAVE) {
			info_length = snprintf(
				info,
				sizeof(info),
				"# Server\r\n"
				"redis_version:0.1.0-kvstore\r\n"
				"redis_mode:standalone\r\n"
				"kvstore_engine:%s\r\n"
				"# Replication\r\n"
				"role:%s\r\n"
				"master_host:%s\r\n"
				"master_port:%u\r\n"
				"master_link_status:%s\r\n",
				kvs_resp_engine_name(),
				kvs_replication_role_name(),
				replication->master_host,
				(unsigned int)replication->master_port,
				kvs_slave_is_connected() ? "up" : "down"
			);
		} else {
			info_length = snprintf(
				info,
				sizeof(info),
				"# Server\r\n"
				"redis_version:0.1.0-kvstore\r\n"
				"redis_mode:standalone\r\n"
				"kvstore_engine:%s\r\n"
				"# Replication\r\n"
				"role:%s\r\n"
				"connected_slaves:%u\r\n",
				kvs_resp_engine_name(),
				kvs_replication_role_name(),
				(unsigned int)kvs_master_slave_count()
			);
		}
		if (info_length < 0 || info_length >= (int)sizeof(info)) return -1;
		int extra = snprintf(info + info_length, sizeof(info) - (size_t)info_length,
			"kvstore_sync_transport:%s\r\nkvstore_replication_port:%u\r\nkvstore_ebpf_enabled:%d\r\n",
			replication->transport == KVS_SYNC_RDMA ? "rdma" : "tcp",
			replication->listen_port ? replication->listen_port : kvs_config_get()->port,
			replication->ebpf_enabled);
		if (extra < 0 || extra >= (int)sizeof(info) - info_length) return -1;
		info_length += extra;
		return kvs_resp_append_bulk(
			response,
			used,
			info,
			(size_t)info_length
		);
	}

	if (kvs_resp_arg_equals(&args[0], "COMMAND")) {
		return kvs_resp_append(response, used, "*0\r\n");
	}

	if (kvs_resp_arg_equals(&args[0], "CLIENT")) {
		if (argc < 2) return kvs_resp_wrong_args(response, used, "client");
		if (kvs_resp_arg_equals(&args[1], "SETINFO") ||
			kvs_resp_arg_equals(&args[1], "SETNAME")) {
			return kvs_resp_append(response, used, "+OK\r\n");
		}
		if (kvs_resp_arg_equals(&args[1], "ID")) {
			return kvs_resp_append(response, used, ":1\r\n");
		}
		if (kvs_resp_arg_equals(&args[1], "GETNAME")) {
			return kvs_resp_append(response, used, "$-1\r\n");
		}
		if (kvs_resp_arg_equals(&args[1], "INFO")) {
			static const char client_info[] =
				"id=1 name= age=0 idle=0 flags=N db=0\r\n";
			return kvs_resp_append_bulk(
				response,
				used,
				client_info,
				strlen(client_info)
			);
		}
		return kvs_resp_append(
			response,
			used,
			"-ERR unsupported CLIENT subcommand\r\n"
		);
	}

	if (kvs_resp_arg_equals(&args[0], "AUTH")) {
		return kvs_resp_append(
			response,
			used,
			"-ERR AUTH called without any password configured\r\n"
		);
	}

	if (kvs_resp_arg_equals(&args[0], "QUIT")) {
		return kvs_resp_append(response, used, "+OK\r\n");
	}

	return kvs_resp_append(
		response,
		used,
		"-ERR unknown command '%.*s'\r\n",
		(int)args[0].length,
		args[0].data
	);
}

int kvs_resp_execute_command(kvs_resp_arg_t *args,int argc,char *response,size_t *used) {
	int lock_mode;
	int result;

	if (args == NULL || argc <= 0) return -1;
	lock_mode = kvs_resp_command_lock_mode(args);

	if ((lock_mode == 2 || kvs_resp_arg_equals(&args[0], "LOAD")) &&
		!kvs_replication_allows_client_writes()) {
		return kvs_resp_append(
			response,
			used,
			"-READONLY You can't write against a read only slave.\r\n"
		);
	}

	if (lock_mode == 2) {
		kvs_store_write_lock();
	} else if (lock_mode == 1) {
		kvs_store_read_lock();
	}

	result = kvs_resp_execute_command_unlocked(args, argc, response, used);

	if (lock_mode != 0) kvs_store_unlock();
	return result;
}
