#include "kvfabric/vllm_kv_cuda.h"
#include <cuda_runtime.h>

template <bool Gather>
__device__ void copy_page(const kvf_cuda_layer *layers, const int32_t *blocks,
                          unsigned layer_count, unsigned tokens, unsigned heads,
                          unsigned head_vectors, uint4 *staging) {
    const kvf_cuda_layer layer = layers[blockIdx.y / 2];
    const size_t offset = size_t(blocks[blockIdx.x]) * layer.block_stride +
                          (blockIdx.y % 2) * layer.side_stride;
    auto *page = static_cast<char *>(layer.cache) + offset;
    const unsigned vectors = tokens * heads * head_vectors;
    auto *linear = staging + (size_t(blockIdx.x) * layer_count * 2 + blockIdx.y) * vectors;
    for (unsigned i = threadIdx.x; i < vectors; i += blockDim.x) {
        const unsigned row = i / head_vectors;
        const size_t row_offset = (row / heads) * layer.token_stride + (row % heads) * layer.head_stride;
        auto *vector = reinterpret_cast<uint4 *>(page + row_offset) + i % head_vectors;
        if constexpr (Gather)
            linear[i] = *vector;
        else
            *vector = linear[i];
    }
}

__global__ void gather_kv_pages(const kvf_cuda_layer *layers, const int32_t *blocks,
                                 unsigned layer_count, unsigned tokens, unsigned heads,
                                 unsigned head_vectors, uint4 *staging) {
    copy_page<true>(layers, blocks, layer_count, tokens, heads, head_vectors, staging);
}

__global__ void scatter_kv_pages(const kvf_cuda_layer *layers, const int32_t *blocks,
                                  unsigned layer_count, unsigned tokens, unsigned heads,
                                  unsigned head_vectors, uint4 *staging) {
    copy_page<false>(layers, blocks, layer_count, tokens, heads, head_vectors, staging);
}

cudaError_t kvf_cuda_gather(const kvf_cuda_layer *layers, const int32_t *blocks,
                            unsigned pages, unsigned layer_count, unsigned tokens,
                            unsigned heads, unsigned head_bytes, void *staging, cudaStream_t stream) {
    if (!pages || !layer_count || layer_count > 32767 || !tokens || !heads ||
        !head_bytes || head_bytes % sizeof(uint4))
        return cudaErrorInvalidValue;
    gather_kv_pages<<<dim3(pages, layer_count * 2), 128, 0, stream>>>(
        layers, blocks, layer_count, tokens, heads, head_bytes / sizeof(uint4),
        static_cast<uint4 *>(staging));
    return cudaGetLastError();
}

cudaError_t kvf_cuda_scatter(const kvf_cuda_layer *layers, const int32_t *blocks,
                             unsigned pages, unsigned layer_count, unsigned tokens,
                             unsigned heads, unsigned head_bytes, void *staging, cudaStream_t stream) {
    if (!pages || !layer_count || layer_count > 32767 || !tokens || !heads ||
        !head_bytes || head_bytes % sizeof(uint4))
        return cudaErrorInvalidValue;
    scatter_kv_pages<<<dim3(pages, layer_count * 2), 128, 0, stream>>>(
        layers, blocks, layer_count, tokens, heads, head_bytes / sizeof(uint4),
        static_cast<uint4 *>(staging));
    return cudaGetLastError();
}
