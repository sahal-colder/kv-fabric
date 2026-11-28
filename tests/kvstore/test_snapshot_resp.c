/*
 * 通用 KVStore 回归层，覆盖 snapshot resp。
 * 夹具拥有本地数据或连接。业务锁由被测代码管理。
 */
/* Round-trip test for the versioned RESP full snapshot. */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/uio.h>
#include <unistd.h>

#include "kvs_store.h"
#include "kvs_config.h"
#include "kvs_allocator.h"
#include "kvs_format.h"

#include "kvs_persistence.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		printf("FAIL: %s\n", msg); \
		failures++; \
	} \
} while (0)

static void populate(void) {
#if ENABLE_ARRAY
	kvs_array_set(&kvs_store.array, "a_key", "a_value");
	kvs_array_set(&kvs_store.array, "empty", "");
#endif
#if ENABLE_RBTREE
	kvs_rbtree_set(&kvs_store.rbtree, "r_key", "r_value");
	kvs_rbtree_set(&kvs_store.rbtree, "r_crlf", "line1\r\nline2");
#endif
#if ENABLE_HASH
	kvs_hash_set(&kvs_store.hash, "h_key", "h_value");
#endif
#if ENABLE_SKIPTABLE
	kvs_skiplist_set(&kvs_store.skiptable, "s_key", "s_value");
#endif
}

static void verify_loaded(void) {
#if ENABLE_ARRAY
	CHECK(kvs_array_get(&kvs_store.array, "a_key") != NULL &&
		strcmp(kvs_array_get(&kvs_store.array, "a_key"), "a_value") == 0,
		"array a_key round-trip");
	CHECK(kvs_array_get(&kvs_store.array, "empty") != NULL &&
		strcmp(kvs_array_get(&kvs_store.array, "empty"), "") == 0,
		"array empty value round-trip");
#endif
#if ENABLE_RBTREE
	CHECK(kvs_rbtree_get(&kvs_store.rbtree, "r_key") != NULL &&
		strcmp(kvs_rbtree_get(&kvs_store.rbtree, "r_key"), "r_value") == 0,
		"rbtree r_key round-trip");
	CHECK(kvs_rbtree_get(&kvs_store.rbtree, "r_crlf") != NULL &&
		strcmp(kvs_rbtree_get(&kvs_store.rbtree, "r_crlf"), "line1\r\nline2") == 0,
		"rbtree value containing CRLF round-trip");
#endif
#if ENABLE_HASH
	CHECK(kvs_hash_get(&kvs_store.hash, "h_key") != NULL &&
		strcmp(kvs_hash_get(&kvs_store.hash, "h_key"), "h_value") == 0,
		"hash h_key round-trip");
#endif
#if ENABLE_SKIPTABLE
	skiptablenode_t *node = kvs_skiplist_get(&kvs_store.skiptable, "s_key");
	CHECK(node != NULL && strcmp(node->value, "s_value") == 0,
		"skiptable s_key round-trip");
#endif
}

static int fail_write;
ssize_t __real_writev(int, const struct iovec *, int);
ssize_t __wrap_writev(int fd, const struct iovec *v, int n) {
    if (fail_write) {
        if (fail_write++ == 1) {
            struct iovec short_write = {v[0].iov_base, v[0].iov_len > 8 ? 8 : v[0].iov_len};
            return __real_writev(fd,&short_write,1);
        }
        errno=EIO;
        return -1;
    }
    return __real_writev(fd,v,n);
}
static void test_file_failure_atomicity(void) {
    char path[]="/tmp/kvf-legacy-snapshot.XXXXXX";
    int fd=mkstemp(path);assert(fd>=0);close(fd);
    assert(!kvs_persistence_save(path));
    fd=open(path,O_RDONLY);assert(fd>=0);
    off_t length=lseek(fd,0,SEEK_END);assert(length>0);assert(lseek(fd,0,SEEK_SET)==0);
    char *saved=malloc((size_t)length+1),*after=malloc((size_t)length);assert(saved&&after);
    assert(read(fd,saved,(size_t)length)==length);saved[length]=0;close(fd);
    assert(!kvs_array_mod(&kvs_store.array,"a_key","modified"));
    fail_write=1;assert(kvs_persistence_save(path)!=0);fail_write=0;
    fd=open(path,O_RDONLY);assert(fd>=0);assert(read(fd,after,(size_t)length)==length);
    assert(lseek(fd,0,SEEK_END)==length && !memcmp(after,saved,(size_t)length));close(fd);
    fd=open(path,O_WRONLY);assert(fd>=0);assert(!ftruncate(fd,length-1));close(fd);
    assert(kvs_persistence_load(path)!=0);assert(!strcmp(kvs_array_get(&kvs_store.array,"a_key"),"modified"));
    fd=open(path,O_WRONLY|O_TRUNC);assert(fd>=0);assert(write(fd,saved,(size_t)length)==length);close(fd);
    /* Payload corruption with intact framing must fail CRC and preserve RAM. */
    char *payload = strstr(saved,"a_value");assert(payload);
    off_t at=(off_t)(payload-saved);fd=open(path,O_WRONLY);assert(fd>=0);
    assert(pwrite(fd,"z",1,at)==1);close(fd);
    assert(kvs_persistence_load(path)!=0);assert(!strcmp(kvs_array_get(&kvs_store.array,"a_key"),"modified"));
    fd=open(path,O_WRONLY|O_TRUNC);assert(fd>=0);assert(write(fd,saved,(size_t)length)==length);close(fd);
    assert(!kvs_persistence_load(path));verify_loaded();
    free(after);free(saved);unlink(path);
}

int main(void) {
	char *data = NULL;
	size_t length = 0;
	const char *snapshot = "build/test_snapshot_resp.snapshot";
	char temporary[128];
	FILE *file;

	kvs_config_t config;
	char error[KVS_CONFIG_ERROR_MAX];
	kvs_config_defaults(&config);
	config.persistence = KVS_PERSISTENCE_FULL;
	if (kvs_config_apply(&config, error) || kvs_allocator_init()) return 1;
	if (kvs_store_create(&kvs_store) != 0) {
		printf("FAIL: engine creation\n");
		kvs_store_destroy(&kvs_store);
		kvs_allocator_destroy();
		return 1;
	}
	populate();

	/* 1. serialize must produce the versioned RESP stream. */
	kvs_store_read_lock();
	int result = kvs_snapshot_export_locked(&data, &length, 0);
	kvs_store_unlock();
	CHECK(result == 0, "serialize succeeds");
	CHECK(length > 0, "snapshot is non-empty");
	{
		kvs_resp_arg_t args[KVS_RESP_MAX_ARGS];
		kvs_format_marker_t marker;
		size_t offset = 0;
		size_t record_length = 0;
		int argc = 0;

		CHECK(kvs_format_parse_resp_record(
			data, length, &offset,
			args, &argc, &record_length
		) == 1 && kvs_format_parse_marker(args, argc, &marker) == 1 &&
			marker.type == KVS_FORMAT_MARKER_META &&
			marker.kind == KVS_FORMAT_KIND_FULL,
			"snapshot starts with full META marker");
	}
	{
		/* the CRLF-containing value must be length-prefixed, not escaped */
		const char needle[] = "$12\r\nline1\r\nline2\r\n";
		int found = 0;
		for (size_t i = 0; i + sizeof(needle) - 1 <= length; ++i) {
			if (memcmp(data + i, needle, sizeof(needle) - 1) == 0) {
				found = 1;
				break;
			}
		}
		CHECK(found, "bulk values are length-prefixed (binary-safe)");
	}

	/* 2. round-trip: deserialize into the globals */
	CHECK(kvs_snapshot_import(data, length) == 0,
		"snapshot import succeeds");
	verify_loaded();

	test_file_failure_atomicity();

	/* 3. truncated stream must fail */
	CHECK(kvs_snapshot_import(data, length - 3) != 0,
		"truncated snapshot rejected");

	/* 4. garbage stream must fail */
	{
		const char junk[] = "not a resp stream at all";
		CHECK(kvs_snapshot_import(junk, sizeof(junk) - 1) != 0,
			"garbage snapshot rejected");
	}

	/* 5. a missing format header is not a valid snapshot. */
	CHECK(kvs_snapshot_import(NULL, 0) != 0,
		"empty snapshot rejected");

	/* A stale .tmp is not a load candidate; only the formal snapshot path is. */
	(void)snprintf(temporary, sizeof(temporary), "%s.tmp", snapshot);
	file = fopen(temporary, "wb");
	if (file != NULL) {
		(void)fputs("not a snapshot", file);
		(void)fclose(file);
	}
	kvs_store_write_lock();
	CHECK(kvs_persistence_load(snapshot) != 0, "missing formal snapshot is rejected");
	kvs_store_unlock();
	(void)remove(temporary);

	kvs_free(data);
	kvs_store_destroy(&kvs_store);
	kvs_allocator_destroy();

	if (failures == 0) {
		printf("SNAPSHOT_RESP_PASS\n");
		return 0;
	}
	printf("SNAPSHOT_RESP_FAIL (%d failures)\n", failures);
	return 1;
}
