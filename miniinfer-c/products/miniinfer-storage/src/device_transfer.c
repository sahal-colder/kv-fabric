/*
 * 设备传输层的 CPU/CUDA 编译期分发。只有调用方拥有分配、流和事件。
 * 锁顺序：本模块无锁。存储层持 Lease 而非持元数据锁调用。
 */
#define _POSIX_C_SOURCE 200809L
#include "miniinfer/device_transfer.h"
#include <stdlib.h>
#include <string.h>

#ifdef MINIINFER_WITH_CUDA
#include <cuda_runtime_api.h>

static mi_status result(cudaError_t code) {
    if (code == cudaSuccess)
        return MI_OK;
    return code == cudaErrorMemoryAllocation ? MI_ENOMEM : MI_EIO;
}
#endif

mi_device_kind mi_device_backend(void) {
#ifdef MINIINFER_WITH_CUDA
    return MI_DEVICE_CUDA;
#else
    return MI_DEVICE_CPU;
#endif
}

mi_status mi_device_select(int device) {
    if (device < 0)
        return MI_OK;
#ifdef MINIINFER_WITH_CUDA
    return result(cudaSetDevice(device));
#else
    return MI_EUNSUPPORTED;
#endif
}

mi_status mi_device_init(int device) {
    return mi_device_select(device);
}

mi_status mi_device_cleanup(int device) {
    /* A pool does not own CUDA's primary context. Other application users may share it. */
    (void)device;
    return MI_OK;
}

mi_status mi_device_synchronize(int device) {
    mi_status st = mi_device_select(device);
#ifdef MINIINFER_WITH_CUDA
    if (st == MI_OK && device >= 0)
        st = result(cudaDeviceSynchronize());
#endif
    return st;
}

mi_status mi_device_host_alloc(int device, size_t bytes, void **out) {
    if (device < 0)
        return posix_memalign(out, 4096, bytes) == 0 ? MI_OK : MI_ENOMEM;
#ifdef MINIINFER_WITH_CUDA
    mi_status st = mi_device_select(device);
    return st == MI_OK ? result(cudaMallocHost(out, bytes)) : st;
#else
    return MI_EUNSUPPORTED;
#endif
}

mi_status mi_device_host_free(int device, void *ptr) {
    if (device < 0) {
        free(ptr);
        return MI_OK;
    }
#ifdef MINIINFER_WITH_CUDA
    mi_status st = mi_device_select(device);
    return st == MI_OK ? result(cudaFreeHost(ptr)) : st;
#else
    return MI_EUNSUPPORTED;
#endif
}

#ifdef MINIINFER_WITH_CUDA
mi_status mi_device_alloc(int device, size_t bytes, void **out) {
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
    if (!stream)
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
    if (!event)
        return MI_OK;
    mi_status st = mi_device_select(device);
    return st == MI_OK ? result(cudaEventDestroy((cudaEvent_t)event)) : st;
}

mi_status mi_device_event_record(int device, mi_event event, mi_stream stream) {
    if (device < 0)
        return MI_OK;
    mi_status st = mi_device_select(device);
    return st == MI_OK ? result(cudaEventRecord((cudaEvent_t)event, (cudaStream_t)stream)) : st;
}

mi_status mi_device_event_wait(int device, mi_event event) {
    if (device < 0)
        return MI_OK;
    mi_status st = mi_device_select(device);
    return st == MI_OK ? result(cudaEventSynchronize((cudaEvent_t)event)) : st;
}

mi_status mi_device_event_ready(int device, mi_event event) {
    if (device < 0)
        return MI_OK;
    mi_status st = mi_device_select(device);
    if (st != MI_OK)
        return st;
    cudaError_t code = cudaEventQuery((cudaEvent_t)event);
    return code == cudaErrorNotReady ? MI_EBUSY : result(code);
}

#else
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

#endif

static mi_status copy(int device, void *dst, const void *src, size_t bytes, mi_stream stream, int to_host) {
    if (device < 0) {
        memmove(dst, src, bytes);
        return MI_OK;
    }
#ifdef MINIINFER_WITH_CUDA
    mi_status st = mi_device_select(device);
    if (st != MI_OK)
        return st;
    enum cudaMemcpyKind direction = to_host ? cudaMemcpyDeviceToHost : cudaMemcpyHostToDevice;
    return result(stream ? cudaMemcpyAsync(dst, src, bytes, direction, (cudaStream_t)stream)
                         : cudaMemcpy(dst, src, bytes, direction));
#else
    (void)stream;
    (void)to_host;
    return MI_EUNSUPPORTED;
#endif
}

mi_status mi_device_copy_to_host(int device, void *host, const void *src, size_t bytes, mi_stream stream) {
    return copy(device, host, src, bytes, stream, 1);
}

mi_status mi_device_copy_from_host(int device, void *dst, const void *host, size_t bytes, mi_stream stream) {
    return copy(device, dst, host, bytes, stream, 0);
}
