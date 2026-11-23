/*
 * 协议层的 RESP 请求和回复接口。
 * 连接借出输入缓冲区，回复由调用方消费。解析器无独立元数据锁。
 */
#include "kvs_resp.h"
#include "kvs_allocator.h"
#include "kvs_network.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int kvs_resp_size_add(size_t *total, size_t value) {
	if (value > SIZE_MAX - *total) return -1;
	*total += value;
	return 0;
}

static size_t kvs_resp_decimal_length(size_t value) {
	size_t length = 1;

	while (value >= 10) {
		value /= 10;
		length++;
	}
	return length;
}

int kvs_resp_build_record(
	const char *const args[],
	int argc,
	char **record,
	size_t *record_size
) {
	char *buffer;
	size_t total;
	size_t used;
	int written;

	if (args == NULL || record == NULL || record_size == NULL ||
		argc <= 0 || argc > KVS_RESP_MAX_ARGS) {
		return -1;
	}

	total = 1 + kvs_resp_decimal_length((size_t)argc) + 2;
	for (int i = 0; i < argc; i++) {
		size_t length;

		if (args[i] == NULL) return -1;
		length = strlen(args[i]);
		if (length > UINT_MAX) return -1;
		if (length > SIZE_MAX - 2) return -1;
		if (kvs_resp_size_add(
				&total,
				1 + kvs_resp_decimal_length(length) + 2
			) != 0 ||
			kvs_resp_size_add(&total, length + 2) != 0) {
			return -1;
		}
	}
	if (total == SIZE_MAX) return -1;

	buffer = kvs_malloc(total + 1);
	if (buffer == NULL) return -1;

	written = snprintf(buffer, total + 1, "*%d\r\n", argc);
	if (written < 0) {
		kvs_free(buffer);
		return -1;
	}
	used = (size_t)written;
	for (int i = 0; i < argc; i++) {
		size_t length = strlen(args[i]);

		written = snprintf(
			buffer + used,
			total + 1 - used,
			"$%u\r\n",
			(unsigned int)length
		);
		if (written < 0 || (size_t)written >= total + 1 - used) {
			kvs_free(buffer);
			return -1;
		}
		used += (size_t)written;
		memcpy(buffer + used, args[i], length);
		used += length;
		memcpy(buffer + used, "\r\n", 2);
		used += 2;
	}
	if (used != total) {
		kvs_free(buffer);
		return -1;
	}
	buffer[used] = '\0';
	*record = buffer;
	*record_size = total;
	return 0;
}

int kvs_resp_append(
	char *response,
	size_t *used,
	const char *format,
	...
) {
	va_list args;
	int written;

	if (*used >= BUFFER_LENGTH) return -1;
	va_start(args, format);
	written = vsnprintf(
		response + *used,
		BUFFER_LENGTH - *used,
		format,
		args
	);
	va_end(args);

	if (written < 0 || (size_t)written >= BUFFER_LENGTH - *used) {
		return -1;
	}
	*used += (size_t)written;
	return 0;
}

int kvs_resp_append_bulk(
	char *response,
	size_t *used,
	const char *value,
	size_t length
) {
	if (kvs_resp_append(response, used, "$%zu\r\n", length) != 0) {
		return -1;
	}
	if (*used + length + 2 >= BUFFER_LENGTH) return -1;
	memcpy(response + *used, value, length);
	*used += length;
	response[(*used)++] = '\r';
	response[(*used)++] = '\n';
	response[*used] = '\0';
	return 0;
}

int kvs_resp_append_value(
	char *response,
	size_t *used,
	const char *value
) {
	if (value == NULL) {
		return kvs_resp_append(response, used, "$-1\r\n");
	}
	return kvs_resp_append_bulk(response, used, value, strlen(value));
}

int kvs_resp_arg_equals(
	const kvs_resp_arg_t *arg,
	const char *name
) {
	size_t length = strlen(name);
	if (arg->length != length) return 0;

	for (size_t i = 0; i < length; i++) {
		if (toupper((unsigned char)arg->data[i]) !=
			toupper((unsigned char)name[i])) {
			return 0;
		}
	}
	return 1;
}
