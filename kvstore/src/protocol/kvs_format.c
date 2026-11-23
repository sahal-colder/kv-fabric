/*
 * 格式层。定义通用 Store 的命令与快照字段编码。
 * 编解码借用调用者的缓冲区。无内部锁。
 */
#include "kvs_format.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t kvs_crc32_update(uint32_t crc, const void *data, size_t length) {
	const unsigned char *bytes = data;
	uint32_t state = ~crc;

	for (size_t i = 0; i < length; i++) {
		state ^= bytes[i];
		for (int bit = 0; bit < 8; bit++) {
			state = (state >> 1) ^
				(0xedb88320U & (uint32_t)-(int32_t)(state & 1U));
		}
	}
	return ~state;
}

uint32_t kvs_crc32(const void *data, size_t length) {
	return kvs_crc32_update(0, data, length);
}

int kvs_format_parse_resp_record(
	const char *data,
	size_t length,
	size_t *offset,
	kvs_resp_arg_t args[KVS_RESP_MAX_ARGS],
	int *argc,
	size_t *record_length
) {
	size_t start;
	size_t cursor;
	size_t count = 0;
	int has_digits = 0;

	if (data == NULL || offset == NULL || args == NULL || argc == NULL ||
		record_length == NULL || *offset > length) {
		return -1;
	}
	start = *offset;
	if (start == length) return 0;
	cursor = start;
	if (data[cursor++] != '*') return -1;

	while (cursor < length && data[cursor] >= '0' && data[cursor] <= '9') {
		unsigned int digit = (unsigned int)(data[cursor] - '0');

		has_digits = 1;
		if (count > (SIZE_MAX - digit) / 10) return -1;
		count = count * 10 + digit;
		if (count > KVS_RESP_MAX_ARGS) return -1;
		cursor++;
	}
	if (cursor == length) return 0;
	if (!has_digits || count == 0 || data[cursor] != '\r') return -1;
	if (length - cursor < 2) return 0;
	if (data[cursor + 1] != '\n') return -1;
	cursor += 2;

	for (size_t index = 0; index < count; index++) {
		size_t bulk_length = 0;

		has_digits = 0;
		if (cursor == length) return 0;
		if (data[cursor++] != '$') return -1;
		while (cursor < length && data[cursor] >= '0' &&
			data[cursor] <= '9') {
			unsigned int digit = (unsigned int)(data[cursor] - '0');

			has_digits = 1;
			if (bulk_length > (SIZE_MAX - digit) / 10) return -1;
			bulk_length = bulk_length * 10 + digit;
			cursor++;
		}
		if (cursor == length) return 0;
		if (!has_digits || data[cursor] != '\r') return -1;
		if (length - cursor < 2) return 0;
		if (data[cursor + 1] != '\n') return -1;
		cursor += 2;

		if (bulk_length > length - cursor) return 0;
		if (length - cursor - bulk_length < 2) {
			if (length - cursor - bulk_length == 1 && data[cursor + bulk_length] != '\r')
				return -1;
			return 0;
		}
		if (data[cursor + bulk_length] != '\r' ||
			data[cursor + bulk_length + 1] != '\n') {
			return -1;
		}
		args[index].data = data + cursor;
		args[index].length = bulk_length;
		cursor += bulk_length + 2;
	}

	*offset = cursor;
	*argc = (int)count;
	*record_length = cursor - start;
	return 1;
}

int kvs_format_arg_to_string(
	const kvs_resp_arg_t *arg,
	char *output,
	size_t capacity
) {
	if (arg == NULL || arg->data == NULL || output == NULL ||
		capacity <= arg->length || memchr(arg->data, 0, arg->length)) {
		return -1;
	}
	memcpy(output, arg->data, arg->length);
	output[arg->length] = '\0';
	return 0;
}

int kvs_format_build_marker(
	const kvs_format_marker_t *marker,
	char **record,
	size_t *record_size
) {
	const char *args[2] = {"PING", NULL};
	const char *kind;
	char payload[160];
	int written;

	if (marker == NULL || record == NULL || record_size == NULL) return -1;
	switch (marker->type) {
	case KVS_FORMAT_MARKER_META:
		if (marker->version == 0) return -1;
		if (marker->kind == KVS_FORMAT_KIND_FULL) {
			kind = "full";
		} else if (marker->kind == KVS_FORMAT_KIND_INCREMENTAL) {
			kind = "incr";
		} else {
			return -1;
		}
		written = snprintf(
			payload,
			sizeof(payload),
			"KVS-META version=%u kind=%s checksum=crc32",
			(unsigned int)marker->version,
			kind
		);
		break;
	case KVS_FORMAT_MARKER_END:
		written = snprintf(
			payload,
			sizeof(payload),
			"KVS-END length=%zu crc32=%08X",
			marker->length,
			(unsigned int)marker->crc32
		);
		break;
	case KVS_FORMAT_MARKER_CHECKPOINT:
		written = snprintf(
			payload,
			sizeof(payload),
			"KVS-CHECKPOINT length=%zu crc32=%08X",
			marker->length,
			(unsigned int)marker->crc32
		);
		break;
	default:
		return -1;
	}
	if (written < 0 || (size_t)written >= sizeof(payload)) return -1;
	args[1] = payload;
	return kvs_resp_build_record(args, 2, record, record_size);
}

int kvs_format_parse_marker(
	const kvs_resp_arg_t *args,
	int argc,
	kvs_format_marker_t *marker
) {
	kvs_format_marker_t parsed = {0};
	char payload[160];
	const char *cursor;
	char *end;

	if (args == NULL || marker == NULL || argc <= 0 ||
		args[0].data == NULL) {
		return -1;
	}
	if (!kvs_resp_arg_equals(&args[0], "PING") || argc < 2) return 0;
	if (args[1].data == NULL) return -1;
	if (args[1].length < 4 || memcmp(args[1].data, "KVS-", 4) != 0) {
		return 0;
	}
	if (argc != 2) return -1;
	if (kvs_format_arg_to_string(&args[1], payload, sizeof(payload)) != 0) {
		return -1;
	}

	if (strncmp(payload, "KVS-META version=", 17) == 0) {
		unsigned long version;

		cursor = payload + 17;
		if (*cursor < '0' || *cursor > '9') return -1;
		errno = 0;
		/*strtoul string to unsigned long*/
		version = strtoul(cursor, &end, 10);
		if (errno == ERANGE || end == cursor || version == 0 ||
			version > UINT32_MAX || strncmp(end, " kind=", 6) != 0) {
			return -1;
		}
		cursor = end + 6;
		if (strncmp(cursor, "full checksum=crc32", 19) == 0 &&
			cursor[19] == '\0') {
			parsed.kind = KVS_FORMAT_KIND_FULL;
		} else if (strncmp(cursor, "incr checksum=crc32", 19) == 0 &&
			cursor[19] == '\0') {
			parsed.kind = KVS_FORMAT_KIND_INCREMENTAL;
		} else {
			return -1;
		}
		parsed.type = KVS_FORMAT_MARKER_META;
		parsed.version = (uint32_t)version;
		*marker = parsed;
		return 1;
	}

	if (strncmp(payload, "KVS-END length=", 15) == 0) {
		parsed.type = KVS_FORMAT_MARKER_END;
		cursor = payload + 15;
	} else if (strncmp(payload, "KVS-CHECKPOINT length=", 22) == 0) {
		parsed.type = KVS_FORMAT_MARKER_CHECKPOINT;
		cursor = payload + 22;
	} else {
		return -1;
	}

	{
		unsigned long long length_value;

		if (*cursor < '0' || *cursor > '9') return -1;
		errno = 0;
		length_value = strtoull(cursor, &end, 10);
		if (errno == ERANGE || end == cursor || length_value > SIZE_MAX ||
			strncmp(end, " crc32=", 7) != 0) {
			return -1;
		}
		parsed.length = (size_t)length_value;
		cursor = end + 7;
	}

	if (strlen(cursor) != 8) return -1;
	parsed.crc32 = 0;
	for (int index = 0; index < 8; index++) {
		unsigned int digit;
		char character = cursor[index];

		if (character >= '0' && character <= '9') {
			digit = (unsigned int)(character - '0');
		} else if (character >= 'A' && character <= 'F') {
			digit = (unsigned int)(character - 'A') + 10U;
		} else if (character >= 'a' && character <= 'f') {
			digit = (unsigned int)(character - 'a') + 10U;
		} else {
			return -1;
		}
		parsed.crc32 = (parsed.crc32 << 4) | digit;
	}
	*marker = parsed;
	return 1;
}

int kvs_format_command_engine(const char *command) {
	const char *operation;
	int engine;

	if (command == NULL || command[0] == '\0') {
		return KVS_FORMAT_ENGINE_UNKNOWN;
	}
	operation = command;
	engine = KVS_FORMAT_ENGINE_ARRAY;
	if (command[0] == 'R') {
		operation = command + 1;
		engine = KVS_FORMAT_ENGINE_RBTREE;
	} else if (command[0] == 'H') {
		operation = command + 1;
		engine = KVS_FORMAT_ENGINE_HASH;
	} else if (command[0] == 'S' && strcmp(command, "SET") != 0) {
		operation = command + 1;
		engine = KVS_FORMAT_ENGINE_SKIPTABLE;
	}

	if (strcmp(operation, "SET") != 0 &&
		strcmp(operation, "MOD") != 0 &&
		strcmp(operation, "DEL") != 0 &&
		strcmp(operation, "FLUSH") != 0) {
		return KVS_FORMAT_ENGINE_UNKNOWN;
	}
	return engine;
}

int kvs_format_build_select(
	int engine,
	char **record,
	size_t *record_size
) {
	static const char *const databases[] = {"0", "1", "2", "3"};
	const char *args[2];

	if (record == NULL || record_size == NULL) return -1;
	*record = NULL;
	*record_size = 0;
	if (engine < KVS_FORMAT_ENGINE_ARRAY ||
		engine > KVS_FORMAT_ENGINE_SKIPTABLE) {
		return -1;
	}
	args[0] = "SELECT";
	args[1] = databases[engine];
	return kvs_resp_build_record(args, 2, record, record_size);
}

int kvs_format_build_mutation(
	const char *command,
	const char *key,
	const char *value,
	char **record,
	size_t *record_size
) {
	const char *mutation_args[3];
	const char *operation;
	int mutation_argc;
	int engine;

	if (record == NULL || record_size == NULL) return -1;
	*record = NULL;
	*record_size = 0;
	engine = kvs_format_command_engine(command);
	if (engine < KVS_FORMAT_ENGINE_ARRAY ||
		engine > KVS_FORMAT_ENGINE_SKIPTABLE) {
		return -1;
	}
	operation = engine == KVS_FORMAT_ENGINE_ARRAY ? command : command + 1;

	if (strcmp(operation, "SET") == 0 || strcmp(operation, "MOD") == 0) {
		if (key == NULL || value == NULL) return -1;
		mutation_args[0] = "SET";
		mutation_args[1] = key;
		mutation_args[2] = value;
		mutation_argc = 3;
	} else if (strcmp(operation, "DEL") == 0) {
		if (key == NULL) return -1;
		mutation_args[0] = "DEL";
		mutation_args[1] = key;
		mutation_argc = 2;
	} else {
		mutation_args[0] = "FLUSHDB";
		mutation_argc = 1;
	}
	return kvs_resp_build_record(
		mutation_args, mutation_argc, record, record_size
	);
}
