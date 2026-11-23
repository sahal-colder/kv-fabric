/*
 * 模型文件层。验证布局并创建只读映射。
 * 映射对象拥有 mmap 区间，张量视图借用地址。无内部锁。
 */
#define _POSIX_C_SOURCE 200809L
#include "miniinfer/model_mapping.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* 校验普通文件的长度后建立只读映射。 */
mi_status mi_model_map(const char *path, size_t min_bytes, void **data, size_t *bytes) {
    if (!path || !data || !bytes)
        return MI_EINVAL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? MI_ENOENT : MI_EIO;

    struct stat st;
    mi_status status = MI_OK;
    if (fstat(fd, &st) != 0)
        status = MI_EIO;
    else if (!S_ISREG(st.st_mode) || st.st_size <= 0 || (uintmax_t)st.st_size < min_bytes)
        status = MI_EINVAL;
    else if ((uintmax_t)st.st_size > SIZE_MAX)
        status = MI_EOVERFLOW;
    if (status != MI_OK) {
        close(fd);
        return status;
    }
    void *mapping = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    int error = errno;
    close(fd);
    if (mapping == MAP_FAILED)
        return error == ENOMEM ? MI_ENOMEM : MI_EIO;
    *data = mapping;
    *bytes = (size_t)st.st_size;
    return MI_OK;
}

void mi_model_unmap(void *data, size_t bytes) {
    if (bytes)
        (void)munmap(data, bytes);
}
