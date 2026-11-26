/*
 * 协议层的传统文本命令解析与执行。
 * 命令切片由调用方缓冲区承载。先取 Store 锁再执行索引或持久化调用。
 */
#include "kvs_protocol.h"
#include "kvs_engine.h"
#include "kvs_config.h"
#include "kvs_format.h"
#include "kvs_replication.h"
#include "kvs_persistence.h"

#include <string.h>

#include <strings.h>

int kvs_text_command(char *line, char response[BUFFER_LENGTH]) {
	char *args[4], *cursor;
	size_t argc = 0;
	for (char *arg = strtok_r(line, " \t\r", &cursor); arg;
	     arg = strtok_r(NULL, " \t\r", &cursor)) {
		if (argc == sizeof(args) / sizeof(*args))
			return sprintf(response, "ERROR\r\n");
		args[argc++] = arg;
	}
	if (!argc)
		return sprintf(response, "ERROR\r\n");

	const char *command = args[0];
	if (argc == 1 && !strcasecmp(command, "PING"))
		return sprintf(response, "+PONG\r\n");
	/* This protocol has no authenticated administrative session. Internal
	 * startup/checkpoint APIs remain available; remote filenames are forbidden. */
	if (!strcasecmp(command, "LOAD") || !strcasecmp(command, "SAVE"))
		return sprintf(response, "ERROR remote persistence disabled\r\n");

	int engine = KVS_FORMAT_ENGINE_ARRAY;
	switch (*command) {
	case 'R': engine = KVS_FORMAT_ENGINE_RBTREE; break;
	case 'H': engine = KVS_FORMAT_ENGINE_HASH; break;
	case 'S':
		if (strcmp(command, "SET"))
			engine = KVS_FORMAT_ENGINE_SKIPTABLE;
		break;
	}
	const char *operation = engine == KVS_FORMAT_ENGINE_ARRAY ? command : command + 1;
	int get = !strcmp(operation, "GET");
	int exists = !strcmp(operation, "EXIST");
	if (get || exists) {
		if (argc != 2)
			return sprintf(response, "ERROR\r\n");
		kvs_store_read_lock();
		const char *value = kvs_store_get(&kvs_store, engine, args[1]);
		int length = get && value ? snprintf(response, BUFFER_LENGTH, "%s\r\n", value) :
		                            sprintf(response, value ? "EXIST\r\n" : "NO EXIST\r\n");
		kvs_store_unlock();
		return length >= BUFFER_LENGTH ? sprintf(response, "ERROR\r\n") : length;
	}

	kvs_mutation_t mutation;
	if (!strcmp(operation, "SET") && argc == 3)
		mutation = KVS_INSERT;
	else if (!strcmp(operation, "MOD") && argc == 3)
		mutation = KVS_UPDATE;
	else if (!strcmp(operation, "DEL") && argc == 2)
		mutation = KVS_DELETE;
	else
		return sprintf(response, "ERROR\r\n");
	if (!kvs_replication_allows_client_writes())
		return sprintf(response, "READONLY\r\n");

	kvs_store_write_lock();
	int result = kvs_engine_mutate_locked(engine, mutation, args[1], argc == 3 ? args[2] : NULL,
	                                      KVS_EVENT_SOURCE_CLIENT);
	kvs_store_unlock();
	return sprintf(response, result < 0 ? "ERROR\r\n" : result == 0 ? "OK\r\n" :
	                         mutation == KVS_INSERT ? "EXIST\r\n" : "NO EXIST\r\n");
}
