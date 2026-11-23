/*
 * 文件 I/O 边界。处理短写、同步文件和同目录原子替换。
 * 调用方持有文件数据，模块管理临时路径和描述符。无内部元数据锁。
 */
#define _GNU_SOURCE
#include "kvs_file.h"
#include "kvs_allocator.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/uio.h>
#include <unistd.h>

int kvs_file_sync(int fd) {
	int result;
	do {
		result = fsync(fd);
	} while (result < 0 && errno == EINTR);
	return result;
}

static int open_parent(const char *path, size_t prefix) {
	if (!prefix)
		return open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	char directory[PATH_MAX];
	if (prefix >= sizeof(directory)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(directory, path, prefix);
	directory[prefix] = '\0';
	return open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

int kvs_file_sync_parent(const char *path) {
	const char *slash = strrchr(path, '/');
	int fd = open_parent(path, slash ? (size_t)(slash + 1 - path) : 0);
	if (fd < 0)
		return -1;
	int result = kvs_file_sync(fd);
	int error = errno;
	close(fd);
	errno = error;
	return result;
}

int kvs_file_replace(const char *path, const void *data, size_t length, int *replacement) {
	const char *slash = strrchr(path, '/');
	const char *name = slash ? slash + 1 : path;
	char temporary[] = ".kvs-XXXXXX";
	size_t prefix = (size_t)(name - path);
	char *pathname = kvs_malloc(prefix + sizeof(temporary));
	if (!pathname) {
		errno = ENOMEM;
		return -1;
	}
	int dir = open_parent(path, prefix);
	if (dir < 0) {
		kvs_free(pathname);
		return -1;
	}
	memcpy(pathname, path, prefix);
	memcpy(pathname + prefix, temporary, sizeof(temporary));
	int fd = mkostemp(pathname, O_CLOEXEC | (replacement ? O_APPEND : 0));
	int result = -1;
	int error;
	if (fd < 0)
		goto done;
	if (replacement && flock(fd, LOCK_EX | LOCK_NB))
		goto done;
	for (size_t offset = 0; offset < length;) {
		struct iovec chunk = {(char *)data + offset, length - offset};
		ssize_t count = writev(fd, &chunk, 1);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (!count)
				errno = EIO;
			goto done;
		}
		offset += (size_t)count;
	}
	if (kvs_file_sync(fd) || renameat(dir, pathname + prefix, dir, name))
		goto done;
	if (replacement) {
		*replacement = fd;
		fd = -1;
	}
	result = kvs_file_sync(dir);
	/* The temp name no longer belongs to us after the rename. */
	pathname[prefix] = '\0';
done:
	error = errno;
	if (fd >= 0) {
		if (close(fd) && !result) {
			result = -1;
			error = errno;
		}
		if (pathname[prefix])
			unlinkat(dir, pathname + prefix, 0);
	}
	close(dir);
	kvs_free(pathname);
	errno = error;
	return result;
}
