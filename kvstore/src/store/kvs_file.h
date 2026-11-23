/*
 * 文件 I/O 边界。处理短写、同步文件和同目录原子替换。
 * 调用方持有文件数据，模块管理临时路径和描述符。无内部元数据锁。
 */
#ifndef KVS_FILE_H
#define KVS_FILE_H

#include <stddef.h>

int kvs_file_sync(int fd);
int kvs_file_sync_parent(const char *path);
/* If replacement is requested, the locked append fd is returned after rename,
 * even if the subsequent directory sync fails. Otherwise no fd is returned. */
int kvs_file_replace(const char *path, const void *data, size_t length, int *replacement);

#endif
