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

static int reserve_reply(kvs_resp_buffer *reply, size_t needed) {
    if (!reply || reply->used > reply->capacity || needed > KVS_RESP_MAX_REPLY)
        return -1;
    if (needed <= reply->capacity)
        return reply->data ? 0 : -1;
    if (!reply->grow)
        return -1;
    size_t next = reply->capacity ? reply->capacity : BUFFER_LENGTH;
    while (next < needed) {
        if (next > KVS_RESP_MAX_REPLY / 2) {
            next = KVS_RESP_MAX_REPLY;
            break;
        }
        next *= 2;
    }
    char *data = kvs_malloc(next);
    if (!data)
        return -1;
    if (reply->used)
        memcpy(data, reply->data, reply->used);
    if (reply->owned)
        kvs_free(reply->data);
    reply->data = data;
    reply->capacity = next;
    reply->owned = 1;
    return 0;
}

static int append_reply(kvs_resp_buffer *reply, const char *format, va_list args) {
    if (!reply || !format || reply->used > reply->capacity || (reply->capacity && !reply->data))
        return -1;
    size_t available = reply->capacity - reply->used;
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(available ? reply->data + reply->used : NULL, available, format, copy);
    va_end(copy);
    if (length < 0)
        return -1;
    if ((size_t)length >= available) {
        if (reply->used > SIZE_MAX - (size_t)length - 1 ||
            reserve_reply(reply, reply->used + (size_t)length + 1))
            return -1;
        int written = vsnprintf(reply->data + reply->used, reply->capacity - reply->used, format, args);
        if (written != length)
            return -1;
    }
    reply->used += (size_t)length;
    return 0;
}

int kvs_resp_buffer_append(kvs_resp_buffer *reply, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = append_reply(reply, format, args);
    va_end(args);
    return result;
}

int kvs_resp_buffer_bulk(kvs_resp_buffer *reply, const char *value, size_t length) {
    char header[32];
    int prefix = snprintf(header, sizeof(header), "$%zu\r\n", length);
    if (!reply || (length && !value) || prefix < 0 || (size_t)prefix >= sizeof(header) ||
        length > SIZE_MAX - (size_t)prefix - 3 ||
        reply->used > SIZE_MAX - length - (size_t)prefix - 3 ||
        reserve_reply(reply, reply->used + length + (size_t)prefix + 3))
        return -1;
    memcpy(reply->data + reply->used, header, (size_t)prefix);
    reply->used += (size_t)prefix;
    if (length)
        memcpy(reply->data + reply->used, value, length);
    reply->used += length;
    memcpy(reply->data + reply->used, "\r\n", 3);
    reply->used += 2;
    return 0;
}

int kvs_resp_buffer_value(kvs_resp_buffer *reply, const char *value) {
    return value ? kvs_resp_buffer_bulk(reply, value, strlen(value)) :
                   kvs_resp_buffer_append(reply, "$-1\r\n");
}

int kvs_resp_append(char *response, size_t *used, const char *format, ...) {
    if (!response || !used)
        return -1;
    kvs_resp_buffer reply = {.data=response, .used=*used, .capacity=BUFFER_LENGTH};
    va_list args;
    va_start(args, format);
    int result = append_reply(&reply, format, args);
    va_end(args);
    *used = reply.used;
    return result;
}

int kvs_resp_append_bulk(char *response, size_t *used, const char *value, size_t length) {
    if (!response || !used)
        return -1;
    kvs_resp_buffer reply = {.data=response, .used=*used, .capacity=BUFFER_LENGTH};
    int result = kvs_resp_buffer_bulk(&reply, value, length);
    *used = reply.used;
    return result;
}
int kvs_resp_append_value(char *response, size_t *used, const char *value) {
    return value ? kvs_resp_append_bulk(response,used,value,strlen(value)) :
                   kvs_resp_append(response,used,"$-1\r\n");
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
