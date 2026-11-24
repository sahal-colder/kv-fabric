/*
 * CPU 设备操作与公共设备接口。调用方拥有分配、流和事件。
 * 锁顺序：本模块无锁。存储层持 Lease 而非持元数据锁调用。
 */
#define _POSIX_C_SOURCE 200809L
#include "miniinfer/device_transfer.h"
#include "miniinfer/cuda_adapter.h"
#include <stdlib.h>
#include <string.h>

mi_status mi_device_init(int device) {
    return mi_device_select(device);
}

mi_status mi_device_cleanup(int device) {
    /* A pool does not own CUDA's primary context. Other application users may share it. */
    (void)device;
    return MI_OK;
}

#ifndef MINIINFER_WITH_CUDA
mi_device_kind mi_device_backend(void) {
    return MI_DEVICE_CPU;
}

mi_status mi_device_select(int device) {
    return device < 0 ? MI_OK : MI_EUNSUPPORTED;
}

mi_status mi_device_synchronize(int device) {
    return mi_device_select(device);
}

mi_status mi_device_host_alloc(int device, size_t bytes, void **out) {
    if (device >= 0)
        return MI_EUNSUPPORTED;
    return posix_memalign(out, 4096, bytes) == 0 ? MI_OK : MI_ENOMEM;
}

mi_status mi_device_host_free(int device, void *ptr) {
    if (device >= 0)
        return MI_EUNSUPPORTED;
    free(ptr);
    return MI_OK;
}

mi_status mi_device_alloc(int device, size_t bytes, void **out) {
    return mi_device_host_alloc(device, bytes, out);
}

mi_status mi_device_free(int device, void *ptr) {
    return mi_device_host_free(device, ptr);
}

mi_status mi_device_stream_create(int device, mi_stream *out) {
    *out = NULL;
    return mi_device_select(device);
}

mi_status mi_device_stream_destroy(int device, mi_stream stream) {
    (void)stream;
    return mi_device_select(device);
}

mi_status mi_device_stream_wait(int device, mi_stream stream) {
    return mi_device_stream_destroy(device, stream);
}

mi_status mi_device_event_create(int device, mi_event *out) {
    *out = NULL;
    return mi_device_select(device);
}

mi_status mi_device_event_destroy(int device, mi_event event) {
    (void)event;
    return mi_device_select(device);
}

mi_status mi_device_event_record(int device, mi_event event, mi_stream stream) {
    (void)stream;
    return mi_device_event_destroy(device, event);
}

mi_status mi_device_event_wait(int device, mi_event event) {
    return mi_device_event_destroy(device, event);
}

mi_status mi_device_event_ready(int device, mi_event event) {
    return mi_device_event_wait(device, event);
}

static mi_status copy(int device, void *dst, const void *src, size_t bytes) {
    if (device >= 0)
        return MI_EUNSUPPORTED;
    memmove(dst, src, bytes);
    return MI_OK;
}

mi_status mi_device_copy_to_host(int device, void *host, const void *src, size_t bytes, mi_stream stream) {
    (void)stream;
    return copy(device, host, src, bytes);
}

mi_status mi_device_copy_from_host(int device, void *dst, const void *host, size_t bytes, mi_stream stream) {
    (void)stream;
    return copy(device, dst, host, bytes);
}
#endif

/* Keep the public CUDA-only adapters available in both builds. */
mi_status mi_cuda_copy_to_host(int device, void *host, const void *src,
                               size_t bytes, mi_stream stream) {
    return mi_device_backend() == MI_DEVICE_CUDA
        ? mi_device_copy_to_host(device, host, src, bytes, stream) : MI_EUNSUPPORTED;
}

mi_status mi_cuda_copy_from_host(int device, void *dst, const void *host,
                                 size_t bytes, mi_stream stream) {
    return mi_device_backend() == MI_DEVICE_CUDA
        ? mi_device_copy_from_host(device, dst, host, bytes, stream) : MI_EUNSUPPORTED;
}

mi_status mi_cuda_wait(int device, mi_stream stream) {
    return mi_device_backend() == MI_DEVICE_CUDA ? mi_device_stream_wait(device, stream) : MI_EUNSUPPORTED;
}

mi_status mi_cuda_stream_create(int device, mi_stream *out) {
    if (!out)
        return MI_EINVAL;
    return mi_device_backend() == MI_DEVICE_CUDA ? mi_device_stream_create(device, out) : MI_EUNSUPPORTED;
}

void mi_cuda_stream_destroy(int device, mi_stream stream) {
    if (mi_device_backend() == MI_DEVICE_CUDA)
        (void)mi_device_stream_destroy(device, stream);
}

mi_status mi_cuda_wait_event(int device, mi_stream stream) {
    return mi_cuda_wait(device, stream);
}
