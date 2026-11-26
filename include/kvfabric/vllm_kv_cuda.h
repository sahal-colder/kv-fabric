#ifndef KVFABRIC_VLLM_KV_CUDA_H
#define KVFABRIC_VLLM_KV_CUDA_H

#include <cuda_runtime_api.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *cache;
    uint64_t block_stride;
    uint64_t side_stride;
    uint64_t token_stride;
    uint64_t head_stride;
} kvf_cuda_layer;

/* Device tables and staging outlive work on stream. Each head row's bytes, base
 * and strides are 16-byte aligned. Block IDs are unique and in bounds.
 * Linear layout is [page][layer][K/V][side_bytes]. */
cudaError_t kvf_cuda_gather(const kvf_cuda_layer *layers, const int32_t *blocks,
                            unsigned pages, unsigned layer_count, unsigned tokens,
                            unsigned heads, unsigned head_bytes,
                            void *staging, cudaStream_t stream);
cudaError_t kvf_cuda_scatter(const kvf_cuda_layer *layers, const int32_t *blocks,
                             unsigned pages, unsigned layer_count, unsigned tokens,
                            unsigned heads, unsigned head_bytes,
                             void *staging, cudaStream_t stream);

#ifdef __cplusplus
}
#endif
#endif
