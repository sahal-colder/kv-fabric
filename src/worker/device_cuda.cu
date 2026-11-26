/*
 * CUDA Runtime backend. Callers retain ownership of allocations, streams and events.
 * Device addresses stay opaque to C11 callers.
 */
#include "miniinfer/device_transfer.h"
#include <cuda_runtime_api.h>
#include <stdlib.h>
#include <string.h>

static mi_status result(cudaError_t code) {
    if (code == cudaSuccess)
        return MI_OK;
    return code == cudaErrorMemoryAllocation ? MI_ENOMEM : MI_EIO;
}

mi_device_kind mi_device_backend(void) {
    return MI_DEVICE_CUDA;
}

mi_status mi_device_select(int device) {
    return device < 0 ? MI_OK : result(cudaSetDevice(device));
}

mi_status mi_device_synchronize(int device) {
    mi_status st = mi_device_select(device);
    if (st == MI_OK && device >= 0)
        st = result(cudaDeviceSynchronize());
    return st;
}

mi_status mi_device_host_alloc(int device, size_t bytes, void **out) {
    if (!out || !bytes)
        return MI_EINVAL;
    *out = NULL;
    if (device < 0)
        return posix_memalign(out, 4096, bytes) == 0 ? MI_OK : MI_ENOMEM;
    mi_status st = mi_device_select(device);
    return st == MI_OK ? result(cudaMallocHost(out, bytes)) : st;
}

mi_status mi_device_host_free(int device, void *ptr) {
    if (device < 0) {
        free(ptr);
        return MI_OK;
    }
    mi_status st = mi_device_select(device);
    return st == MI_OK ? result(cudaFreeHost(ptr)) : st;
}

mi_status mi_device_alloc(int device, size_t bytes, void **out) {
    if (!out || !bytes)
        return MI_EINVAL;
    *out = NULL;
    if (device < 0)
        return mi_device_host_alloc(device, bytes, out);
    mi_status st = mi_device_select(device);
    return st == MI_OK ? result(cudaMalloc(out, bytes)) : st;
}

mi_status mi_device_free(int device, void *ptr) {
    if (device < 0)
        return mi_device_host_free(device, ptr);
    mi_status st = mi_device_select(device);
    return st == MI_OK ? result(cudaFree(ptr)) : st;
}

mi_status mi_device_stream_create(int device, mi_stream *out) {
    if (!out)
        return MI_EINVAL;
    *out = NULL;
    if (device < 0)
        return MI_OK;
    mi_status st = mi_device_select(device);
    cudaStream_t stream;
    if (st == MI_OK)
        st = result(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    if (st == MI_OK)
        *out = stream;
    return st;
}

mi_status mi_device_stream_destroy(int device, mi_stream stream) {
    if (device < 0 || !stream)
        return MI_OK;
    mi_status st = mi_device_select(device);
    return st == MI_OK ? result(cudaStreamDestroy((cudaStream_t)stream)) : st;
}

mi_status mi_device_stream_wait(int device, mi_stream stream) {
    if (device < 0)
        return MI_OK;
    mi_status st = mi_device_select(device);
    return st == MI_OK ? result(cudaStreamSynchronize((cudaStream_t)stream)) : st;
}

mi_status mi_device_event_create(int device, mi_event *out) {
    if (!out)
        return MI_EINVAL;
    *out = NULL;
    if (device < 0)
        return MI_OK;
    mi_status st = mi_device_select(device);
    cudaEvent_t event;
    if (st == MI_OK)
        st = result(cudaEventCreateWithFlags(&event, cudaEventDisableTiming));
    if (st == MI_OK)
        *out = event;
    return st;
}

mi_status mi_device_event_destroy(int device, mi_event event) {
    if (device < 0 || !event)
        return MI_OK;
    mi_status st = mi_device_select(device);
    return st == MI_OK ? result(cudaEventDestroy((cudaEvent_t)event)) : st;
}

mi_status mi_device_event_record(int device, mi_event event, mi_stream stream) {
    if (device >= 0 && !event)
        return MI_EINVAL;
    if (device < 0)
        return MI_OK;
    mi_status st = mi_device_select(device);
    return st == MI_OK ? result(cudaEventRecord((cudaEvent_t)event, (cudaStream_t)stream)) : st;
}

mi_status mi_device_event_wait(int device, mi_event event) {
    if (device >= 0 && !event)
        return MI_EINVAL;
    if (device < 0)
        return MI_OK;
    mi_status st = mi_device_select(device);
    return st == MI_OK ? result(cudaEventSynchronize((cudaEvent_t)event)) : st;
}

mi_status mi_device_event_ready(int device, mi_event event) {
    if (device >= 0 && !event)
        return MI_EINVAL;
    if (device < 0)
        return MI_OK;
    mi_status st = mi_device_select(device);
    if (st != MI_OK)
        return st;
    cudaError_t code = cudaEventQuery((cudaEvent_t)event);
    return code == cudaErrorNotReady ? MI_EBUSY : result(code);
}

static mi_status copy(int device, void *dst, const void *src, size_t bytes, mi_stream stream, int to_host) {
    if (!bytes)
        return MI_OK;
    if (!dst || !src)
        return MI_EINVAL;
    if (device < 0) {
        memmove(dst, src, bytes);
        return MI_OK;
    }
    mi_status st = mi_device_select(device);
    if (st != MI_OK)
        return st;
    enum cudaMemcpyKind direction = to_host ? cudaMemcpyDeviceToHost : cudaMemcpyHostToDevice;
    return result(stream ? cudaMemcpyAsync(dst, src, bytes, direction, (cudaStream_t)stream)
                         : cudaMemcpy(dst, src, bytes, direction));
}

mi_status mi_device_copy_to_host(int device, void *host, const void *src, size_t bytes, mi_stream stream) {
    return copy(device, host, src, bytes, stream, 1);
}

mi_status mi_device_copy_from_host(int device, void *dst, const void *host, size_t bytes, mi_stream stream) {
    return copy(device, dst, host, bytes, stream, 0);
}
