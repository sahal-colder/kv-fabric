#include "kvfabric/vllm_kv_cuda.h"
#include <cuda_runtime.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <string_view>
#include <vector>

static void check(cudaError_t status) {
    if (status != cudaSuccess) {
        std::fprintf(stderr, "%s\n", cudaGetErrorString(status));
        std::exit(1);
    }
}

int main(int argc, char **argv) {
    constexpr unsigned pages = 128, layers = 36, side_bytes = 8192, physical_pages = 256;
    constexpr size_t layer_bytes = 2 * side_bytes;
    constexpr size_t page_bytes = layers * layer_bytes;
    constexpr size_t bytes = pages * page_bytes;
    const std::string_view requested = argc == 2 ? argv[1] : "all";
    if (requested != "all" && requested != "dma" && requested != "segments" && requested != "vector") {
        std::fprintf(stderr, "usage: kvf_cuda_bench [all|dma|segments|vector]\n");
        return 2;
    }
    FILE *raw = nullptr;
    if (const char *path = std::getenv("KVF_CUDA_RAW_CSV")) {
        raw = std::fopen(path, "w");
        if (!raw) { std::perror(path); return 1; }
        std::fputs("mode,direction,sample,payload_bytes,payload_memcpy_count,elapsed_ms\n", raw);
    }
    unsigned char *input, *output, *packed, *src, *dst;
    kvf_cuda_layer *device_layers;
    int32_t *device_ids;
    cudaStream_t stream;
    cudaEvent_t start, stop;
    check(cudaMallocHost(&input, bytes));
    check(cudaMallocHost(&output, bytes));
    check(cudaMalloc(&packed, bytes));
    check(cudaMalloc(&src, physical_pages * page_bytes));
    check(cudaMalloc(&dst, physical_pages * page_bytes));
    check(cudaMalloc(&device_layers, 2 * layers * sizeof(*device_layers)));
    check(cudaMalloc(&device_ids, pages * sizeof(*device_ids)));
    check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    check(cudaEventCreate(&start));
    check(cudaEventCreate(&stop));

    std::vector<int32_t> blocks(physical_pages);
    std::iota(blocks.begin(), blocks.end(), 0);
    std::mt19937 random(734);
    std::shuffle(blocks.begin(), blocks.end(), random);
    std::vector<kvf_cuda_layer> table(2 * layers);
    for (unsigned l = 0; l < layers; ++l) {
        table[l] = {src + l * layer_bytes, page_bytes, side_bytes, 512, 256};
        table[l + layers] = {dst + l * layer_bytes, page_bytes, side_bytes, 512, 256};
    }
    check(cudaMemcpy(device_ids, blocks.data(), pages * sizeof(*device_ids), cudaMemcpyHostToDevice));
    check(cudaMemcpy(device_layers, table.data(), table.size() * sizeof(*device_layers), cudaMemcpyHostToDevice));
    for (size_t i = 0; i < bytes; ++i)
        input[i] = static_cast<unsigned char>((i / side_bytes) ^ (i / 31) ^ (i / 65535));
    for (unsigned p = 0; p < pages; ++p)
        check(cudaMemcpy(src + blocks[p] * page_bytes, input + p * page_bytes, page_bytes, cudaMemcpyHostToDevice));

    for (std::string_view mode : {"dma", "segments", "vector"}) {
        if (requested != "all" && requested != mode)
            continue;
        auto transfer = [&](bool upload) {
            if (mode == "dma") {
                check(upload ? cudaMemcpyAsync(dst, input, bytes, cudaMemcpyHostToDevice, stream)
                             : cudaMemcpyAsync(output, dst, bytes, cudaMemcpyDeviceToHost, stream));
                return;
            }
            if (mode == "segments") {
                for (unsigned p = 0; p < pages; ++p)
                    for (unsigned l = 0; l < layers; ++l) {
                        size_t offset = l * layer_bytes;
                        check(upload ? cudaMemcpyAsync(dst + blocks[p] * page_bytes + offset,
                                                        input + p * page_bytes + offset, layer_bytes,
                                                        cudaMemcpyHostToDevice, stream)
                                     : cudaMemcpyAsync(output + p * page_bytes + offset,
                                                        src + blocks[p] * page_bytes + offset, layer_bytes,
                                                        cudaMemcpyDeviceToHost, stream));
                    }
                return;
            }
            if (upload) {
                check(cudaMemcpyAsync(packed, input, bytes, cudaMemcpyHostToDevice, stream));
                check(kvf_cuda_scatter(device_layers + layers, device_ids, pages, layers,
                                      16, 2, 256, packed, stream));
            } else {
                check(kvf_cuda_gather(device_layers, device_ids, pages, layers, 16, 2, 256, packed, stream));
                check(cudaMemcpyAsync(output, packed, bytes, cudaMemcpyDeviceToHost, stream));
            }
        };
        for (bool upload : {true, false}) {
            for (unsigned i = 0; i < 3; ++i) {
                transfer(upload);
                check(cudaStreamSynchronize(stream));
            }
            std::vector<float> samples;
            for (unsigned i = 0; i < 15; ++i) {
                check(cudaEventRecord(start, stream));
                transfer(upload);
                check(cudaEventRecord(stop, stream));
                check(cudaEventSynchronize(stop));
                float ms;
                check(cudaEventElapsedTime(&ms, start, stop));
                samples.push_back(ms);
                if (raw) std::fprintf(raw, "%s,%s,%u,%zu,%u,%.6f\n", mode.data(),
                    upload ? "H2D" : "D2H", i, bytes, mode == "segments" ? pages * layers : 1, ms);
            }
            std::sort(samples.begin(), samples.end());
            float ms = samples[samples.size() / 2];
            if (upload) {
                for (unsigned p = 0; p < pages; ++p) {
                    const auto index = mode == "dma" ? p : blocks[p];
                    check(cudaMemcpy(output + p * page_bytes, dst + index * page_bytes,
                                     page_bytes, cudaMemcpyDeviceToHost));
                }
            }
            if (std::memcmp(input, output, bytes)) {
                std::fprintf(stderr, "%s %s byte mismatch\n", mode.data(), upload ? "H2D" : "D2H");
                return 1;
            }
            std::printf("%s,%s,%u,%zu,%.4f,%.3f\n", mode.data(), upload ? "H2D" : "D2H",
                        mode == "segments" ? pages * layers : 1, bytes, ms, bytes / (ms * 1e-3 * (1ull << 30)));
        }
    }
    check(cudaEventDestroy(stop));
    check(cudaEventDestroy(start));
    check(cudaStreamDestroy(stream));
    check(cudaFree(device_ids));
    check(cudaFree(device_layers));
    check(cudaFree(dst));
    check(cudaFree(src));
    check(cudaFree(packed));
    check(cudaFreeHost(output));
    check(cudaFreeHost(input));
    if (raw && std::fclose(raw)) { std::perror("CUDA raw CSV close"); return 1; }
}

