/*
 * 格式层。定义通用 Store 的命令与快照字段编码。
 * 编解码借用调用者的缓冲区。无内部锁。
 */
#ifndef KVS_FORMAT_H
#define KVS_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#include "kvs_resp.h"
#include "kvs_mutation.h"

#define KVS_FORMAT_VERSION 1U

/* Keep these values aligned with KVS_PERSISTENCE_* for config integration. */
#define KVS_FORMAT_KIND_UNKNOWN		(-1)
#define KVS_FORMAT_KIND_FULL		0
#define KVS_FORMAT_KIND_INCREMENTAL	1

#define KVS_FORMAT_MARKER_UNKNOWN	0
#define KVS_FORMAT_MARKER_META		1
#define KVS_FORMAT_MARKER_END		2
#define KVS_FORMAT_MARKER_CHECKPOINT	3

typedef struct kvs_format_marker_s {
	int type;
	uint32_t version;
	int kind;
	size_t length;
	uint32_t crc32;
} kvs_format_marker_t;

/*
 * CRC-32/ISO-HDLC (reflected polynomial 0xEDB88320).
 *
 * Pass 0 for the first chunk and feed each returned value into the next call
 * when calculating a checksum incrementally.  data must be non-NULL whenever
 * length is greater than zero.
 */
uint32_t kvs_crc32_update(uint32_t crc, const void *data, size_t length);
uint32_t kvs_crc32(const void *data, size_t length);

/*
 * Parse one RESP2 array whose elements are bulk strings without modifying
 * data.  On success, offset advances past the record and record_length is the
 * exact number of source bytes occupied by that record.
 *
 * Return values:
 *   1  - one complete record was parsed
 *   0  - no complete record is available yet
 *  -1  - invalid input or malformed RESP
 *
 * args and argc are valid only when 1 is returned.  offset is changed only on
 * success, which lets callers retry after receiving more bytes.
 */
int kvs_format_parse_resp_record(
	const char *data,
	size_t length,
	size_t *offset,
	kvs_resp_arg_t args[KVS_RESP_MAX_ARGS],
	int *argc,
	size_t *record_length
);

/* Copy one non-NUL-terminated RESP argument into a caller-owned C string. */
int kvs_format_arg_to_string(
	const kvs_resp_arg_t *arg,
	char *output,
	size_t capacity
);

/*
 * Build metadata as a Redis-compatible `PING <payload>` RESP2 command.
 * META uses version/kind; END and CHECKPOINT use length/crc32.
 * The caller owns the allocated record returned by build_marker.
 */
int kvs_format_build_marker(
	const kvs_format_marker_t *marker,
	char **record,
	size_t *record_size
);

/*
 * Parse one command previously split into RESP arguments.
 * Return 1 for a valid KVS marker, 0 for a regular command, and -1 for a
 * malformed or unknown command in the reserved KVS marker namespace.
 */
int kvs_format_parse_marker(
	const kvs_resp_arg_t *args,
	int argc,
	kvs_format_marker_t *marker
);

/* Return the engine selected by an internal write command, or -1. */
int kvs_format_command_engine(const char *command);

/* Build `SELECT <engine>` as one RESP2 command. */
int kvs_format_build_select(
	int engine,
	char **record,
	size_t *record_size
);

/*
 * Convert one internal mutation into one standard Redis RESP2 `SET`, `DEL`,
 * or `FLUSHDB` command.  Engine selection is emitted separately so callers
 * can group commands by engine.  The caller owns the allocated record.
 */
int kvs_format_build_mutation(
	const char *command,
	const char *key,
	const char *value,
	char **record,
	size_t *record_size
);

#endif
