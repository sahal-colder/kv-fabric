/*
 * 持久化层。编码、读取并原子替换快照。
 * 文件和临时 Store 由操作持有。序列化的共享 Store 由其读写锁保护。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kvs_store.h"
#include "kvs_persistence_internal.h"
#include "kvs_file.h"
#include "kvs_config.h"
#include "kvs_allocator.h"
#include "kvs_format.h"

typedef struct snapshot_buffer_s {
	unsigned char *data;
	size_t length;
	size_t capacity;
	size_t limit; /* 0 for file snapshots; replication supplies a memory budget. */
} snapshot_buffer_t;

static int append(snapshot_buffer_t *buffer, const void *data, size_t length) {
	size_t required;
	size_t capacity;
	unsigned char *new_data;

	if (length > SIZE_MAX - buffer->length) return -1;
	required = buffer->length + length;
	if (buffer->limit && required > buffer->limit) return -1;
	if (required > buffer->capacity) {
		capacity = buffer->capacity ? buffer->capacity : 4096;
		while (capacity < required) {
			if (capacity > SIZE_MAX / 2) {
				capacity = required;
				break;
			}
			capacity *= 2;
		}
		if (buffer->limit && capacity > buffer->limit) capacity = buffer->limit;
		new_data = (unsigned char *)kvs_realloc(buffer->data, capacity);
		if (new_data == NULL) return -1;
		buffer->data = new_data;
		buffer->capacity = capacity;
	}
	memcpy(buffer->data + buffer->length, data, length);
	buffer->length = required;
	return 0;
}

static int append_resp_select(snapshot_buffer_t *buffer, int engine) {
	char *record = NULL;
	size_t record_size = 0;
	int result;

	if (kvs_format_build_select(engine, &record, &record_size) != 0) {
		return -1;
	}
	result = append(buffer, record, record_size);
	kvs_free(record);
	return result;
}

static int append_resp_mutation(
	snapshot_buffer_t *buffer,
	const char *command,
	const char *key,
	const char *value
) {
	char *record = NULL;
	size_t record_size = 0;
	int result;

	if ((key && strlen(key) > KVS_MAX_FIELD_BYTES) ||
	    (value && strlen(value) > KVS_MAX_FIELD_BYTES))
		return -1;

	if (kvs_format_build_mutation(
			command, key, value, &record, &record_size
		) != 0) {
		return -1;
	}
	result = append(buffer, record, record_size);
	kvs_free(record);
	return result;
}

#if ENABLE_RBTREE
static int save_rbtree_resp(snapshot_buffer_t *buffer, const rbtree *tree,
		const rbtree_node *node) {
	if (node == NULL || node == tree->nil) return 0;
	if (save_rbtree_resp(buffer, tree, node->left) != 0 ||
		append_resp_mutation(
			buffer, "RSET", node->key, node->value
		) != 0 ||
		save_rbtree_resp(buffer, tree, node->right) != 0) {
		return -1;
	}
	return 0;
}
#endif

static int serialize_resp(snapshot_buffer_t *buffer) {
	kvs_format_marker_t marker = {
		.type = KVS_FORMAT_MARKER_META,
		.version = KVS_FORMAT_VERSION,
		.kind = KVS_FORMAT_KIND_FULL
	};
	char *record = NULL;
	size_t record_size = 0;
	size_t payload_offset;
	int result;

	if (kvs_format_build_marker(&marker, &record, &record_size) != 0) {
		return -1;
	}
	result = append(buffer, record, record_size);
	kvs_free(record);
	record = NULL;
	if (result != 0) return -1;
	payload_offset = buffer->length;

#if ENABLE_ARRAY
	if (append_resp_select(buffer, KVS_FORMAT_ENGINE_ARRAY) != 0 ||
		append_resp_mutation(buffer, "FLUSH", NULL, NULL) != 0) {
		return -1;
	}
	for (int i = 0; i < kvs_store.array.total; i++) {
		if (kvs_store.array.table[i].key != NULL &&
			append_resp_mutation(
				buffer,
				"SET",
				kvs_store.array.table[i].key,
				kvs_store.array.table[i].value
			) != 0) {
			return -1;
		}
	}
#endif
#if ENABLE_RBTREE
	if (append_resp_select(buffer, KVS_FORMAT_ENGINE_RBTREE) != 0 ||
		append_resp_mutation(buffer, "RFLUSH", NULL, NULL) != 0 ||
		save_rbtree_resp(buffer, &kvs_store.rbtree, kvs_store.rbtree.root) != 0) {
		return -1;
	}
#endif
#if ENABLE_HASH
	if (append_resp_select(buffer, KVS_FORMAT_ENGINE_HASH) != 0 ||
		append_resp_mutation(buffer, "HFLUSH", NULL, NULL) != 0) {
		return -1;
	}
	for (int i = 0; i < kvs_store.hash.max_slots; i++) {
		for (hashnode_t *node = kvs_store.hash.nodes[i]; node; node = node->next) {
			if (append_resp_mutation(
					buffer, "HSET", node->key, node->value
				) != 0) {
				return -1;
			}
		}
	}
#endif
#if ENABLE_SKIPTABLE
	if (append_resp_select(buffer, KVS_FORMAT_ENGINE_SKIPTABLE) != 0 ||
		append_resp_mutation(buffer, "SFLUSH", NULL, NULL) != 0) {
		return -1;
	}
	for (skiptablenode_t *node = kvs_store.skiptable.header ?
			kvs_store.skiptable.header->forward[0] : NULL;
			node; node = node->forward[0]) {
		if (append_resp_mutation(
				buffer, "SSET", node->key, node->value
			) != 0) {
			return -1;
		}
	}
#endif

	marker.type = KVS_FORMAT_MARKER_END;
	marker.length = buffer->length - payload_offset;
	marker.crc32 = kvs_crc32(buffer->data + payload_offset, marker.length);
	if (kvs_format_build_marker(&marker, &record, &record_size) != 0) {
		return -1;
	}
	result = append(buffer, record, record_size);
	kvs_free(record);
	return result;
}

int kvs_save(const char *filename) {
	snapshot_buffer_t buffer = {0};
	if (filename == NULL || filename[0] == '\0')
		filename = kvs_config_get()->snapshot_file;
	kvs_store_read_lock();
	int result = serialize_resp(&buffer);
	kvs_store_unlock();
	if (!result && kvs_file_replace(filename, buffer.data, buffer.length, NULL)) {
		perror("snapshot replace");
		result = -1;
	}
	kvs_free(buffer.data);
	return result;
}

static int apply_resp_snapshot_record(
	kvs_store_t *engine,
	int *selected_engine,
	const kvs_resp_arg_t *args,
	int argc
) {
	char *key;
	char *value;
	int set_result = -1;

	if (kvs_resp_arg_equals(&args[0], "SELECT")) {
		if (argc != 2 || args[1].length != 1 ||
			args[1].data[0] < '0' || args[1].data[0] > '3') {
			return -1;
		}
		*selected_engine = args[1].data[0] - '0';
		return 0;
	}
	if (kvs_resp_arg_equals(&args[0], "FLUSHDB")) {
		return argc == 1 &&
			*selected_engine != KVS_FORMAT_ENGINE_UNKNOWN ? 0 : -1;
	}
	if (!kvs_resp_arg_equals(&args[0], "SET") || argc != 3 ||
		*selected_engine == KVS_FORMAT_ENGINE_UNKNOWN ||
		args[1].length > KVS_MAX_FIELD_BYTES ||
		args[2].length > KVS_MAX_FIELD_BYTES) {
		return -1;
	}

	key = kvs_malloc(args[1].length + 1);
	value = kvs_malloc(args[2].length + 1);
	if (key == NULL || value == NULL ||
		kvs_format_arg_to_string(&args[1], key, args[1].length + 1) != 0 ||
		kvs_format_arg_to_string(
			&args[2], value, args[2].length + 1
		) != 0) {
		kvs_free(key);
		kvs_free(value);
		return -1;
	}

	set_result = kvs_store_mutate(engine, *selected_engine, KVS_INSERT, key, value);
	kvs_free(key);
	kvs_free(value);
	return set_result == 0 ? 0 : -1;
}

static int deserialize_and_install(const void *data, size_t length, int replica) {
	const char *bytes = data;
	kvs_resp_arg_t args[KVS_RESP_MAX_ARGS];
	kvs_format_marker_t marker;
	kvs_store_t engine;
	size_t offset = 0;
	size_t record_start;
	size_t record_length;
	size_t payload_start;
	size_t payload_end = 0;
	int argc;
	int marker_status;
	int selected_engine = KVS_FORMAT_ENGINE_UNKNOWN;
	int last_engine = KVS_FORMAT_ENGINE_UNKNOWN;
	int section_flushed = 0;
	int found_end = 0;
	int result = -1;

	if (data == NULL || length == 0 ||
		kvs_format_parse_resp_record(
			bytes, length, &offset, args, &argc, &record_length
		) != 1 ||
		kvs_format_parse_marker(args, argc, &marker) != 1 ||
		marker.type != KVS_FORMAT_MARKER_META ||
		marker.version != KVS_FORMAT_VERSION ||
		marker.kind != KVS_FORMAT_KIND_FULL) {
		return -1;
	}
	payload_start = offset;

	while (offset < length) {
		int engine_number;

		record_start = offset;
		if (kvs_format_parse_resp_record(
				bytes, length, &offset, args, &argc, &record_length
			) != 1) {
			return -1;
		}
		marker_status = kvs_format_parse_marker(args, argc, &marker);
		if (marker_status < 0) return -1;
		if (marker_status == 1) {
			payload_end = record_start;
			if (marker.type != KVS_FORMAT_MARKER_END || offset != length ||
				(selected_engine != KVS_FORMAT_ENGINE_UNKNOWN &&
				 !section_flushed) ||
				marker.length != payload_end - payload_start ||
				kvs_crc32(
					bytes + payload_start, marker.length
				) != marker.crc32) {
				return -1;
			}
			found_end = 1;
			break;
		}

		if (kvs_resp_arg_equals(&args[0], "SELECT")) {
			if (argc != 2 || args[1].length != 1 ||
				args[1].data[0] < '0' || args[1].data[0] > '3' ||
				(selected_engine != KVS_FORMAT_ENGINE_UNKNOWN &&
				 !section_flushed)) {
				return -1;
			}
			engine_number = args[1].data[0] - '0';
			if (engine_number <= last_engine) return -1;
			selected_engine = engine_number;
			last_engine = engine_number;
			section_flushed = 0;
			continue;
		}
		if (kvs_resp_arg_equals(&args[0], "FLUSHDB")) {
			if (argc != 1 || selected_engine == KVS_FORMAT_ENGINE_UNKNOWN ||
				section_flushed) {
				return -1;
			}
			section_flushed = 1;
			continue;
		}
		if (!kvs_resp_arg_equals(&args[0], "SET") || argc != 3 ||
			selected_engine == KVS_FORMAT_ENGINE_UNKNOWN ||
			!section_flushed || args[1].length > KVS_MAX_FIELD_BYTES ||
			args[2].length > KVS_MAX_FIELD_BYTES) {
			return -1;
		}
	}
	if (!found_end || kvs_store_create(&engine) != 0) return -1;

	offset = payload_start;
	selected_engine = KVS_FORMAT_ENGINE_UNKNOWN;
	while (offset < payload_end) {
		if (kvs_format_parse_resp_record(
				bytes, payload_end, &offset, args, &argc, &record_length
			) != 1 ||
			apply_resp_snapshot_record(
				&engine, &selected_engine, args, argc
			) != 0) {
			goto done;
		}
	}

	if (replica)
		kvs_store_write_lock();
	if (!replica || kvs_config_get()->persistence != KVS_PERSISTENCE_INCREMENTAL ||
	    !kvs_incremental_replace(bytes + payload_start, payload_end - payload_start)) {
		kvs_store_swap(&engine);
		result = 0;
	}
	if (replica)
		kvs_store_unlock();
done:
	kvs_store_destroy(&engine);
	return result;
}

int kvs_snapshot_export_locked(char **data, size_t *length, size_t limit) {
	snapshot_buffer_t buffer = { .limit = limit };
	if (!data || !length || serialize_resp(&buffer)) {
		kvs_free(buffer.data);
		return -1;
	}
	*data = (char *)buffer.data;
	*length = buffer.length;
	return 0;
}

int kvs_snapshot_import(const void *data, size_t length) {
	return deserialize_and_install(data, length, 1);
}

int kvs_load(const char *filename) {
	int fd;
	struct stat info;
	void *mapping;
	int result;

	if (filename == NULL || filename[0] == '\0') {
		filename = kvs_config_get()->snapshot_file;
	}
	fd = open(filename, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
		(uint64_t)info.st_size > SIZE_MAX) {
		if (fd >= 0) close(fd);
		return -1;
	}
	mapping = mmap(NULL, (size_t)info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (mapping == MAP_FAILED) {
		return -1;
	}
	result = deserialize_and_install(mapping, (size_t)info.st_size, 0);
	munmap(mapping, (size_t)info.st_size);
	return result;
}
