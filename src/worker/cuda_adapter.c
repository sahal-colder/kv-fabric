/*
 * 旧 CUDA API 的兼容入口。资源依旧归调用方，实际操作交给 Device HAL。
 * 锁顺序：无内部锁，调用方在存储元数据锁外等待设备。
 */
#include "miniinfer/cuda_adapter.h"
#include "miniinfer/device_transfer.h"

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
