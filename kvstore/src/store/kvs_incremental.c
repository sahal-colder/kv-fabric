/*
 * 持久化层。追加、校验和重放 AOF，并支持全量同步的检查点。
 * AOF 后端管理文件。写序由 Store 写锁保护，重放到临时 Store 后交换。
 */
#include "kvs_store.h"
#include "kvs_persistence_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kvs_allocator.h"
#include "kvs_config.h"
#include "kvs_file.h"
#include "kvs_format.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

/* Runtime access is serialized by the store lock. Startup and shutdown have
 * exclusive ownership before starting or after joining the network workers. */
static int aof_fd = -1;
static int aof_engine = KVS_FORMAT_ENGINE_UNKNOWN;

int kvs_incremental_start(void) {
	aof_fd = open(kvs_config_get()->aof_file, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NONBLOCK, 0600);
	if (aof_fd < 0) {
		perror("AOF open");
		return -1;
	}
	if (flock(aof_fd, LOCK_EX | LOCK_NB)) {
		perror("AOF lock");
	} else if (!kvs_incremental_load()) {
		if (!kvs_file_sync_parent(kvs_config_get()->aof_file))
			return 0;
		perror("AOF directory fsync");
	}
	fprintf(stderr, "AOF recovery failed: %s\n", kvs_config_get()->aof_file);
	close(aof_fd);
	aof_fd = -1;
	return -1;
}

int kvs_incremental_stop(void) {
	int result = kvs_incremental_save();
	if (close(aof_fd)) {
		perror("AOF close");
		result = -1;
	}
	aof_fd = -1;
	aof_engine = KVS_FORMAT_ENGINE_UNKNOWN;
	return result;
}

int kvs_incremental_save(void) {
	if (!kvs_file_sync(aof_fd))
		return 0;
	perror("AOF fsync");
	return -1;
}

int kvs_incremental_append(const char *cmd, const char *key, const char *value) {
	char select[] = "*2\r\n$6\r\nSELECT\r\n$1\r\n0\r\n";
	char *mutation;
	size_t length;
	int engine = kvs_format_command_engine(cmd);
	if (kvs_format_build_mutation(cmd, key, value, &mutation, &length))
		return -1;
	select[sizeof(select) - 4] = '0' + engine;
	struct iovec records[] = {
		{select, engine == aof_engine ? 0 : sizeof(select) - 1},
		{mutation, length}
	};
	ssize_t count;
	do {
		count = writev(aof_fd, records, 2);
	} while (count < 0 && errno == EINTR);
	kvs_free(mutation);
	if (count == (ssize_t)(records[0].iov_len + length)) {
		aof_engine = engine;
		return 0;
	}

	if (count < 0) {
		perror("AOF write");
	} else {
		fprintf(stderr, "AOF short write: %zd of %zu bytes\n", count, records[0].iov_len + length);
		/* O_APPEND leaves the file offset at the end of the short write. */
		off_t end = lseek(aof_fd, 0, SEEK_CUR);
		if (end < count || ftruncate(aof_fd, end - count) || kvs_file_sync(aof_fd))
			perror("AOF rollback");
	}
	return -1;
}

static int apply_record(kvs_store_t *store, int *selected, kvs_resp_arg_t *args, int argc) {
	char command[16];
	if (argc > 3 || kvs_format_arg_to_string(&args[0], command, sizeof(command)))
		return -1;
	if (!strcmp(command, "SELECT")) {
		if (argc != 2 || args[1].length != 1 || args[1].data[0] < '0' || args[1].data[0] > '3')
			return -1;
		*selected = args[1].data[0] - '0';
		return 0;
	}

	int engine = kvs_format_command_engine(command);
	const char *name = engine > KVS_FORMAT_ENGINE_ARRAY ? command + 1 : command;
	if (*selected >= 0 && (!strcmp(command, "SET") || !strcmp(command, "DEL") ||
	                      !strcmp(command, "FLUSHDB")))
		engine = *selected;
	if (engine < 0)
		return -1;
	kvs_mutation_t operation = !strcmp(name, "DEL") ? KVS_DELETE :
	                           !strcmp(name, "FLUSH") || !strcmp(name, "FLUSHDB") ? KVS_CLEAR :
	                           !strcmp(name, "MOD") ? KVS_UPDATE : KVS_UPSERT;
	if (argc != (operation == KVS_CLEAR ? 1 : operation == KVS_DELETE ? 2 : 3))
		return -1;
	for (int i = 1; i < argc; i++) {
		if (memchr(args[i].data, 0, args[i].length))
			return -1;
		((char *)args[i].data)[args[i].length] = '\0';
	}
	int result = kvs_store_mutate(store, engine, operation, argc > 1 ? args[1].data : NULL,
	                              argc > 2 ? args[2].data : NULL);
	return result > 0 && operation == KVS_DELETE ? 0 : result;
}

int kvs_incremental_load(void) {
	struct stat info;
	if (fstat(aof_fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 ||
	    (uintmax_t)info.st_size > SIZE_MAX || kvs_incremental_save())
		return -1;
	size_t length = (size_t)info.st_size;
	char *data = NULL;
	if (length) {
		/* Replay terminates bulk strings in a private map, never on disk. */
		data = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE, aof_fd, 0);
		if (data == MAP_FAILED) {
			perror("AOF mmap");
			return -1;
		}
	}
	kvs_store_t staged;
	int result = -1;
	size_t offset = 0;
	int selected = KVS_FORMAT_ENGINE_UNKNOWN;
	if (kvs_store_create(&staged))
		goto unmap;
	while (offset < length) {
		kvs_resp_arg_t args[KVS_RESP_MAX_ARGS];
		size_t record_length;
		int argc;
		int status = kvs_format_parse_resp_record(data, length, &offset, args, &argc, &record_length);
		if (!status)
			break;
		if (status < 0 || apply_record(&staged, &selected, args, argc)) {
			fprintf(stderr, "AOF invalid record or replay failure near byte %zu\n", offset);
			goto done;
		}
	}
	if (offset < length) {
		munmap(data, length);
		data = NULL;
		if (ftruncate(aof_fd, (off_t)offset) || kvs_incremental_save()) {
			perror("AOF truncate");
			goto done;
		}
		fprintf(stderr, "AOF truncated incomplete tail from %zu to %zu bytes\n", length, offset);
	}
	kvs_store_swap(&staged);
	aof_engine = selected;
	result = 0;
done:
	kvs_store_destroy(&staged);
unmap:
	if (data)
		munmap(data, length);
	return result;
}

int kvs_incremental_replace(const void *data, size_t length) {
	int replacement = -1;
	int result = kvs_file_replace(kvs_config_get()->aof_file, data, length, &replacement);
	if (result)
		perror("replica AOF replacement");
	if (replacement < 0)
		return -1;
	/* Once renamed, the old fd can no longer represent the configured log. */
	if (result)
		_Exit(EXIT_FAILURE);
	if (close(aof_fd))
		perror("old AOF close");
	aof_fd = replacement;
	aof_engine = KVS_FORMAT_ENGINE_UNKNOWN;
	return 0;
}
