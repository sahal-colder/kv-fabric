/*
 * MiniInfer 存储回归层，覆盖 gpu mock。
 * 夹具拥有临时存储和缓冲区。线程与租约在销毁前收尾。
 */
#include "miniinfer/miniinfer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef MINIINFER_WITH_CUDA
#include <cuda_runtime_api.h>
#endif

/* CUDA 构建验证流租约、延迟回收及 GPU 与 SSD 迁移。 */
int main(void) {
#ifndef MINIINFER_WITH_CUDA
    mi_mempool *device_pool = NULL;
    assert(mi_mempool_create(MI_MEMORY_CUDA, 0, (mi_mempool_config){0}, &device_pool) == MI_EUNSUPPORTED);
    puts("CUDA-disabled contract ok");
#else
    int count;
    if (cudaGetDeviceCount(&count) != cudaSuccess || !count) {
        puts("No CUDA device available");
        return 77;
    }

    mi_mempool *host, *device, *pinned;
    assert(mi_mempool_create(MI_MEMORY_CPU, -1, (mi_mempool_config){0}, &host) == MI_OK);
    mi_mempool_config device_config = {.max_cached_bytes = 4096, .chunk_bytes = 4096};
    assert(mi_mempool_create(MI_MEMORY_CUDA, 0, device_config, &device) == MI_OK);
    mi_mempool_config pinned_config = {.max_reserved_bytes = 3 * 4096 - 1};
    assert(mi_mempool_create(MI_MEMORY_CUDA_PINNED, 0, pinned_config, &pinned) == MI_OK);
    mi_storage_backend *cpu, *gpu, *ssd;
    assert(mi_memory_backend_create(MI_LOCATION_CPU, 4096, host, 2, &cpu) == MI_OK);
    assert(mi_memory_backend_create(MI_LOCATION_GPU, 4096, device, 2, &gpu) == MI_OK);
    assert(mi_ssd_backend_create(NULL, 4096, 2 * (4096 + 64), MI_SSD_VERIFY_ON_READ, &ssd) == MI_OK);
    mi_storage_config cfg = {.block_bytes = 4096, .cpu_bytes = 4096, .gpu_bytes = 4096,
                             .ssd_bytes = 2 * (4096 + 64), .max_async_migrations = 2};
    mi_tiered_storage *storage;
    mi_storage_backends backends = {.cpu = cpu, .gpu = gpu, .ssd = ssd, .pinned_pool = pinned};
    assert(mi_tiered_storage_create(cfg, backends, &storage) == MI_OK);
    cudaStream_t stream;
    assert(cudaStreamCreate(&stream) == cudaSuccess);
    void *buffer, *reused;
    assert(mi_mempool_alloc(device, 4096, 256, &buffer) == MI_OK);
    assert(cudaMemsetAsync(buffer, 1, 4096, stream) == cudaSuccess);
    assert(mi_mempool_release_async(device, buffer, stream) == MI_OK);
    assert(mi_mempool_release(device, buffer) == MI_EINVAL);
    assert(cudaStreamSynchronize(stream) == cudaSuccess);
    assert(mi_mempool_collect(device) == MI_OK);
    assert(mi_mempool_alloc(device, 4096, 256, &reused) == MI_OK && reused == buffer);
    assert(mi_mempool_release(device, reused) == MI_OK);

    /* 待回收的中间块在事件完成后才与左右两侧合并。 */
    void *left, *middle, *right;
    assert(mi_mempool_alloc(device, 640, 256, &left) == MI_OK && left == buffer);
    assert(mi_mempool_alloc(device, 1025, 256, &middle) == MI_OK);
    assert((uintptr_t)middle == (uintptr_t)left + 768);
    assert(mi_mempool_alloc(device, 2048, 256, &right) == MI_OK);
    assert((uintptr_t)right == (uintptr_t)middle + 1280);
    assert(cudaMemsetAsync(middle, 2, 1025, stream) == cudaSuccess);
    assert(mi_mempool_release_async(device, middle, stream) == MI_OK);
    assert(mi_mempool_release(device, left) == MI_OK);
    assert(mi_mempool_release(device, right) == MI_OK);
    assert(mi_mempool_release(device, middle) == MI_EINVAL);
    assert(cudaStreamSynchronize(stream) == cudaSuccess);
    assert(mi_mempool_collect(device) == MI_OK);
    assert(mi_mempool_alloc(device, 4096, 256, &reused) == MI_OK && reused == buffer);
    assert(mi_mempool_release(device, reused) == MI_OK);

    mi_block_id id;
    mi_lease lease;
    assert(mi_storage_create_block(storage, 4096, MI_LOCATION_GPU, &id) == MI_OK);
    assert(mi_storage_acquire(storage, id, MI_ACCESS_WRITE, NULL, &lease) == MI_EINVAL);
    assert(mi_storage_acquire(storage, id, MI_ACCESS_WRITE, stream, &lease) == MI_OK);
    assert(cudaMemsetAsync(lease.data, 0x6d, lease.size, stream) == cudaSuccess);
    assert(mi_lease_finish(&lease) == MI_OK);

    assert(mi_storage_migrate(storage, id, MI_LOCATION_SSD) == MI_OK);
    assert(mi_storage_migrate(storage, id, MI_LOCATION_GPU) == MI_OK);
    assert(mi_storage_migrate(storage, id, MI_LOCATION_CPU) == MI_OK);
    unsigned char expected[4096], actual[4096];
    memset(expected, 0x6d, sizeof(expected));
    assert(mi_storage_read_to_host(storage, id, actual, sizeof(actual), NULL) == MI_OK);
    assert(!memcmp(actual, expected, sizeof(actual)));
    assert(mi_storage_erase(storage, id) == MI_OK);
    assert(cudaStreamDestroy(stream) == cudaSuccess);
    mi_tiered_storage_destroy(storage);
    void *staging;
    assert(mi_mempool_alloc_slab(pinned, 2 * 4096, 4096, &staging) == MI_OK);
    assert((uintptr_t)staging % 4096 == 0);
    assert(mi_mempool_release(pinned, staging) == MI_OK);
    mi_mempool_destroy(pinned);
    mi_backend_destroy(ssd);
    mi_backend_destroy(gpu);
    mi_backend_destroy(cpu);
    mi_mempool_destroy(device);
    mi_mempool_destroy(host);
    puts("CUDA leases, staging and reuse ok");
#endif
    return 0;
}
