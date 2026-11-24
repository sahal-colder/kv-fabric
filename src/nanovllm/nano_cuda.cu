#include "nano_cuda.h"
#ifdef NANO_WITH_STORAGE
#include "miniinfer/allocator.h"
#include "miniinfer/graph_allocator.h"
#endif
#include <cuda_runtime.h>
#include <math_constants.h>
#include <cub/block/block_reduce.cuh>
#include <climits>
#include <cmath>
#include <cstring>
#include <stdlib.h>

/* CUDA 执行状态持有流、图以及模型、KV 和前向缓冲区。 */
struct NvCuda {
    int device;
    NvConfig cfg;
    NvWeights weights;
    mi_mempool *pool;
#ifdef NANO_WITH_STORAGE
    mi_graph_allocator *workspace;
#endif
    cudaStream_t stream;
#ifdef NANO_WITH_REMOTE_KV
    cudaStream_t upload_stream;
    cudaEvent_t upload_done, snapshot_done;
    unsigned char *upload_stage, *snapshot_stage;
    size_t upload_capacity;
#endif
    cudaGraphExec_t graph;
    void *buffers[6];
    int *table, *input, *host_input;
    float *keys, *values, *rope, *logits, *host_logits;
    float *x, *xb, *xb2, *qkv, *gate, *up;
    int block_tokens, kv, head_size;
};

static void check(cudaError_t error) {
    if (error != cudaSuccess)
        nv_fatal(cudaGetErrorString(error));
}

__global__ static void embed(float *x, const float *weight, const int *input, int dim) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < dim)
        x[i] = weight[(size_t)input[0] * dim + i];
}

__global__ static void normalize(float *out, const float *x, const float *weight, int dim) {
    using Reduce = cub::BlockReduce<float, 128>;
    __shared__ typename Reduce::TempStorage scratch;
    __shared__ float scale;
    float sum = 0;
    for (int i = threadIdx.x; i < dim; i += blockDim.x)
        sum += x[i] * x[i];
    sum = Reduce(scratch).Sum(sum);
    if (threadIdx.x == 0)
        scale = rsqrtf(sum / dim + 1e-5f);
    __syncthreads();
    for (int i = threadIdx.x; i < dim; i += blockDim.x)
        out[i] = (x[i] * scale) * weight[i];
}

/* 每个 warp 算一行。Q/K/V 或 gate/up 可在同一次启动中投影。 */
__global__ static void project(float *out, const float *x, const float *first, const float *second,
                               const float *third, int in, int rows, int first_rows, int second_rows) {
    /* 一个 128 线程块有四个 warp，各负责一行。 */
    int row = blockIdx.x * 4 + threadIdx.x / 32;
    int lane = threadIdx.x % 32;
    if (row >= rows)
        return;
    const float *weight;
    if (row < first_rows)
        weight = first + (size_t)row * in;
    else if (row < first_rows + second_rows)
        weight = second + (size_t)(row - first_rows) * in;
    else
        weight = third + (size_t)(row - first_rows - second_rows) * in;
    float sum = 0;
    if (in % 4 == 0 && ((uintptr_t)weight & 15u) == 0) {
        const float4 *w = reinterpret_cast<const float4 *>(weight);
        const float4 *v = reinterpret_cast<const float4 *>(x);
        for (int i = lane; i < in / 4; i += 32) {
            float4 a = w[i], b = v[i];
            sum += a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        }
    } else {
        for (int i = lane; i < in; i += 32)
            sum += weight[i] * x[i];
    }
    for (int delta = 16; delta; delta /= 2)
        /* 汇总同一 warp 各 lane 的部分点积。 */
        sum += __shfl_down_sync(0xffffffff, sum, delta);
    if (lane == 0)
        out[row] = sum;
}

/* 按物理页、层、页内 token 和通道展开 KV 地址。 */
__device__ static size_t kv_offset(const int *table, int pos, int layer, int layers, int block, int kv) {
    return (((size_t)table[pos / block] * layers + layer) * block + pos % block) * kv;
}

__global__ static void rotate_store(float *qkv, float *keys, float *values, const int *table,
                                    const int *input, const float *rope, int dim, int kv, int head,
                                    int layer, int layers, int block) {
    int i = (blockIdx.x * blockDim.x + threadIdx.x) * 2;
    if (i >= dim)
        return;
    int pos = input[1];
    float cs = rope[(size_t)pos * head + i % head];
    float sn = rope[(size_t)pos * head + i % head + 1];
    float a = qkv[i], b = qkv[i + 1];
    qkv[i] = a * cs - b * sn;
    qkv[i + 1] = a * sn + b * cs;
    if (i < kv) {
        size_t index = kv_offset(table, pos, layer, layers, block, kv) + i;
        a = qkv[dim + i];
        b = qkv[dim + i + 1];
        keys[index] = a * cs - b * sn;
        keys[index + 1] = a * sn + b * cs;
        values[index] = qkv[dim + kv + i];
        values[index + 1] = qkv[dim + kv + i + 1];
    }
}

__global__ static void attend(float *out, const float *queries, const float *keys, const float *values,
                              const int *table, const int *input, int layer, int layers, int block,
                              int kv, int head_size, int group) {
    using Reduce = cub::BlockReduce<float, 128>;
    __shared__ typename Reduce::TempStorage scratch;
    __shared__ float max_score, total;
    extern __shared__ float scores[];
    int pos = input[1];
    int head = blockIdx.x;
    /* 同组 Query 头共享同一个 KV 头。 */
    int column = (head / group) * head_size;
    const float *q = queries + head * head_size;
    float maximum = -CUDART_INF_F;
    for (int t = threadIdx.x; t <= pos; t += blockDim.x) {
        const float *k = keys + kv_offset(table, t, layer, layers, block, kv) + column;
        float value = 0;
        for (int i = 0; i < head_size; ++i)
            value += q[i] * k[i];
        value *= rsqrtf((float)head_size);
        scores[t] = value;
        maximum = fmaxf(maximum, value);
    }
    maximum = Reduce(scratch).Reduce(maximum, cub::Max());
    if (threadIdx.x == 0)
        max_score = maximum;
    __syncthreads();
    float sum = 0;
    for (int t = threadIdx.x; t <= pos; t += blockDim.x) {
        /* 以当前注意力头的最大得分为基准做稳定 Softmax。 */
        scores[t] = expf(scores[t] - max_score);
        sum += scores[t];
    }
    sum = Reduce(scratch).Sum(sum);
    if (threadIdx.x == 0)
        total = sum;
    __syncthreads();
    for (int i = threadIdx.x; i < head_size; i += blockDim.x) {
        float value = 0;
        for (int t = 0; t <= pos; ++t)
            value += (scores[t] / total) * values[kv_offset(table, t, layer, layers, block, kv) + column + i];
        out[(size_t)head * head_size + i] = value;
    }
}

__global__ static void add(float *x, const float *delta, int dim) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < dim)
        x[i] += delta[i];
}

__global__ static void swiglu(float *gate, const float *up, int dim) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < dim)
        gate[i] = (gate[i] / (1.0f + expf(-gate[i]))) * up[i];
}

/* 将嵌入、各层计算和词表投影按顺序排入同一条流。 */
static void enqueue_forward(NvCuda *g) {
    NvConfig c = g->cfg;
    NvWeights w = g->weights;
    auto s = g->stream;
    int d = c.dim, h = c.hidden_dim, k = g->kv;
    unsigned vectors = (d - 1) / 128 + 1;
    size_t dd = (size_t)d * d, kd = (size_t)k * d, hd = (size_t)h * d;
    embed<<<vectors, 128, 0, s>>>(g->x, w.embedding, g->input, d);
    for (int l = 0; l < c.n_layers; ++l) {
        normalize<<<1, 128, 0, s>>>(g->xb, g->x, w.att_norm + (size_t)l * d, d);
        project<<<(d + 2 * k - 1) / 4 + 1, 128, 0, s>>>(
            g->qkv, g->xb, w.wq + l * dd, w.wk + l * kd, w.wv + l * kd, d, d + 2 * k, d, k);
        rotate_store<<<vectors, 128, 0, s>>>(g->qkv, g->keys, g->values, g->table, g->input,
            g->rope, d, k, g->head_size, l, c.n_layers, g->block_tokens);
        attend<<<c.n_heads, 128, (size_t)c.seq_len * sizeof(float), s>>>(
            g->xb, g->qkv, g->keys, g->values, g->table, g->input, l, c.n_layers,
            g->block_tokens, k, g->head_size, c.n_heads / c.n_kv_heads);
        project<<<(d - 1) / 4 + 1, 128, 0, s>>>(g->xb2, g->xb, w.wo + l * dd, nullptr, nullptr, d, d, d, 0);
        add<<<vectors, 128, 0, s>>>(g->x, g->xb2, d);
        normalize<<<1, 128, 0, s>>>(g->xb, g->x, w.ffn_norm + (size_t)l * d, d);
        project<<<(2 * h - 1) / 4 + 1, 128, 0, s>>>(
            g->gate, g->xb, w.w1 + l * hd, w.w3 + l * hd, nullptr, d, 2 * h, h, h);
        swiglu<<<(h - 1) / 128 + 1, 128, 0, s>>>(g->gate, g->up, h);
        project<<<(d - 1) / 4 + 1, 128, 0, s>>>(g->xb2, g->gate, w.w2 + l * hd, nullptr, nullptr, h, d, d, 0);
        add<<<vectors, 128, 0, s>>>(g->x, g->xb2, d);
    }
    normalize<<<1, 128, 0, s>>>(g->xb, g->x, w.final_norm, d);
    project<<<(c.vocab_size - 1) / 4 + 1, 128, 0, s>>>(
        g->logits, g->xb, w.classifier, nullptr, nullptr, d, c.vocab_size, c.vocab_size, 0);
}

void nv_cuda_init(NvEngine *engine, int device) {
    NvModel *model = engine->model;
    NvCache *cache = &engine->cache;
    NvConfig c = model->config;
    NvCuda *g = (NvCuda *)nv_alloc(1, sizeof(*g));
    g->device = cache->device = device;
    g->cfg = c;
    g->head_size = c.dim / c.n_heads;
    g->kv = cache->kv_dim;
    g->block_tokens = cache->block_size;
    if (g->kv > (INT_MAX - c.dim) / 2 || c.hidden_dim > INT_MAX / 2)
        nv_fatal("model projection exceeds the CUDA row limit");
    check(cudaSetDevice(device));
    cudaDeviceProp props{};
    check(cudaGetDeviceProperties(&props, device));
    if ((size_t)c.seq_len > props.sharedMemPerBlock / sizeof(float))
        nv_fatal("model context exceeds the CUDA attention workspace");
    check(cudaStreamCreateWithFlags(&g->stream, cudaStreamNonBlocking));
    /* 文件头之后的权重保持原布局，偏移一律以第一项权重为起点。 */
    const auto *payload = static_cast<const unsigned char *>(model->mapping) + sizeof(NvConfig);
    size_t weight_bytes = model->mapping_size - sizeof(NvConfig);
    size_t kv_count = cache->elements;
    size_t table_count = ((size_t)c.seq_len - 1) / cache->block_size + 1;
    auto aligned = [](size_t count) { return (count + 3) & ~(size_t)3; };
    size_t d = aligned(c.dim), h = aligned(c.hidden_dim);
    size_t qkv = aligned((size_t)c.dim + 2 * (size_t)g->kv);
    size_t logits_bytes = (size_t)c.vocab_size * sizeof(float);
    size_t rope_count = (size_t)c.seq_len * g->head_size;
    size_t bytes[] = {weight_bytes, 2 * kv_count * sizeof(float), (table_count + 2) * sizeof(int),
        (3 * d + qkv + 2 * h) * sizeof(float), logits_bytes, rope_count * sizeof(float)};
#ifdef NANO_WITH_STORAGE
    if (engine->options.pooled_buffers) {
        size_t total = 0;
        for (size_t i = 0; i < 6; ++i) {
            if (bytes[i] > SIZE_MAX - 255 || ((bytes[i] + 255) & ~(size_t)255) > SIZE_MAX - total)
                nv_fatal("CUDA buffer size overflow");
            total += (bytes[i] + 255) & ~(size_t)255;
        }
        auto require = [](mi_status st) {
            if (st != MI_OK)
                nv_fatal(mi_status_string(st));
        };
        require(mi_mempool_create(MI_MEMORY_CUDA, device, mi_mempool_config{}, &g->pool));
        require(mi_graph_allocator_create(g->pool, total, 6, &g->workspace));
        require(mi_graph_begin_capture(g->workspace, 1, g->stream));
        for (size_t i = 0; i < 6; ++i) {
            require(mi_graph_alloc(g->workspace, bytes[i], g->stream, &g->buffers[i]));
            require(mi_graph_release(g->workspace, g->buffers[i], g->stream));
        }
        require(mi_graph_end_capture(g->workspace, 1));
    } else
#endif
        for (size_t i = 0; i < 6; ++i)
            check(cudaMalloc(&g->buffers[i], bytes[i]));
    auto *weights = static_cast<unsigned char *>(g->buffers[0]);
    check(cudaMemcpyAsync(weights, payload, weight_bytes, cudaMemcpyHostToDevice, g->stream));
    auto mapped = [payload, weights](const float *ptr) {
        return reinterpret_cast<float *>(weights + (reinterpret_cast<const unsigned char *>(ptr) - payload));
    };
    NvWeights w = model->weights;
    g->weights = NvWeights{mapped(w.embedding), mapped(w.att_norm), mapped(w.wq), mapped(w.wk),
        mapped(w.wv), mapped(w.wo), mapped(w.ffn_norm), mapped(w.w1), mapped(w.w2), mapped(w.w3),
        mapped(w.final_norm), mapped(w.classifier)};

    g->keys = static_cast<float *>(g->buffers[1]);
    g->values = g->keys + kv_count;
    g->input = static_cast<int *>(g->buffers[2]);
    g->table = g->input + 2;
    check(cudaMallocHost(reinterpret_cast<void **>(&g->host_input), bytes[2]));
    g->x = static_cast<float *>(g->buffers[3]);
    g->xb = g->x + d;
    g->xb2 = g->xb + d;
    g->qkv = g->xb2 + d;
    g->gate = g->qkv + qkv;
    g->up = g->gate + c.hidden_dim;
    g->logits = static_cast<float *>(g->buffers[4]);
    check(cudaMallocHost(reinterpret_cast<void **>(&g->host_logits), logits_bytes));

    float *rope = (float *)nv_alloc(rope_count, sizeof(*rope));
    for (int pos = 0; pos < c.seq_len; ++pos) {
        for (int i = 0; i < g->head_size; i += 2) {
            float angle = (float)pos / powf(10000.0f, (float)i / (float)g->head_size);
            size_t index = (size_t)pos * g->head_size + i;
            rope[index] = cosf(angle);
            rope[index + 1] = sinf(angle);
        }
    }
    g->rope = static_cast<float *>(g->buffers[5]);
    check(cudaMemcpyAsync(g->rope, rope, bytes[5], cudaMemcpyHostToDevice, g->stream));
    check(cudaStreamSynchronize(g->stream));
    free(rope);
    check(cudaStreamBeginCapture(g->stream, cudaStreamCaptureModeThreadLocal));
    enqueue_forward(g);
    check(cudaMemcpyAsync(g->host_logits, g->logits, logits_bytes, cudaMemcpyDeviceToHost, g->stream));
    cudaGraph_t graph;
    check(cudaStreamEndCapture(g->stream, &graph));
    check(cudaGraphInstantiate(&g->graph, graph, 0));
    check(cudaGraphDestroy(graph));
    check(cudaGraphUpload(g->graph, g->stream));
    check(cudaStreamSynchronize(g->stream));
    cache->cuda = g;
}

float *nv_cuda_forward(NvCache *cache, const int *table, int token, int pos) {
    auto *g = cache->cuda;
    check(cudaSetDevice(g->device));
    size_t n = (size_t)pos / cache->block_size + 1;
    g->host_input[0] = token;
    g->host_input[1] = pos;
    std::memcpy(g->host_input + 2, table, n * sizeof(int));
    check(cudaMemcpyAsync(g->input, g->host_input, (n + 2) * sizeof(int), cudaMemcpyHostToDevice, g->stream));
    check(cudaGraphLaunch(g->graph, g->stream));
    check(cudaStreamSynchronize(g->stream));
    return g->host_logits;
}

void nv_cuda_copy_block(NvCache *cache, int block, void *host, size_t half, int to_device) {
    auto *g = cache->cuda;
    check(cudaSetDevice(g->device));
    float *device[] = {g->keys, g->values};
    for (int i = 0; i < 2; ++i) {
        void *a = device[i] + (size_t)block * (half / sizeof(float));
        void *b = static_cast<unsigned char *>(host) + i * half;
        check(cudaMemcpyAsync(to_device ? a : b, to_device ? b : a, half,
                              to_device ? cudaMemcpyHostToDevice : cudaMemcpyDeviceToHost, g->stream));
    }
    check(cudaStreamSynchronize(g->stream));
}

#ifdef NANO_WITH_REMOTE_KV
unsigned char *nv_cuda_remote_stage(NvCache *cache, size_t bytes) {
    NvCuda *g = cache->cuda;
    if (bytes > g->upload_capacity) {
        unsigned char *stage;
        if (cudaSetDevice(g->device) != cudaSuccess ||
            cudaMallocHost((void **)&stage, bytes) != cudaSuccess)
            return NULL;
        if (g->upload_stage)
            (void)cudaFreeHost(g->upload_stage);
        g->upload_stage = stage;
        g->upload_capacity = bytes;
    }
    return g->upload_stage;
}

int nv_cuda_remote_init(NvCache *cache, size_t export_bytes, size_t import_bytes) {
    NvCuda *g = cache->cuda;
    return cudaSetDevice(g->device) == cudaSuccess &&
           cudaStreamCreateWithFlags(&g->upload_stream, cudaStreamNonBlocking) == cudaSuccess &&
           cudaEventCreateWithFlags(&g->upload_done, cudaEventDisableTiming | cudaEventBlockingSync) == cudaSuccess &&
           cudaEventCreateWithFlags(&g->snapshot_done, cudaEventDisableTiming) == cudaSuccess &&
           cudaMallocHost((void **)&g->snapshot_stage, export_bytes) == cudaSuccess &&
           nv_cuda_remote_stage(cache, import_bytes);
}

void nv_cuda_remote_free(NvCache *cache) {
    NvCuda *g = cache->cuda;
    (void)cudaSetDevice(g->device);
    if (g->snapshot_done)
        (void)cudaEventDestroy(g->snapshot_done);
    if (g->upload_done)
        (void)cudaEventDestroy(g->upload_done);
    if (g->upload_stream)
        (void)cudaStreamDestroy(g->upload_stream);
    if (g->snapshot_stage)
        (void)cudaFreeHost(g->snapshot_stage);
    if (g->upload_stage)
        (void)cudaFreeHost(g->upload_stage);
    g->snapshot_done = g->upload_done = NULL;
    g->upload_stream = NULL;
    g->snapshot_stage = g->upload_stage = NULL;
    g->upload_capacity = 0;
}

int nv_cuda_remote_snapshot(NvCache *cache, int block, void *host, size_t half) {
    NvCuda *g = cache->cuda;
    if (cudaSetDevice(g->device) != cudaSuccess)
        return 0;
    float *sides[] = {g->keys, g->values};
    cudaError_t status = cudaSuccess;
    for (int i = 0; i < 2 && status == cudaSuccess; ++i) {
        void *device = sides[i] + (size_t)block * (half / sizeof(float));
        status = cudaMemcpyAsync(g->snapshot_stage + (size_t)i * half, device, half,
                                 cudaMemcpyDeviceToHost, g->stream);
    }
    if (status == cudaSuccess)
        status = cudaEventRecord(g->snapshot_done, g->stream);
    if (status == cudaSuccess)
        status = cudaEventSynchronize(g->snapshot_done);
    if (status != cudaSuccess)
        (void)cudaStreamSynchronize(g->stream);
    else
        memcpy(host, g->snapshot_stage, 2 * half);
    return status == cudaSuccess;
}

int nv_cuda_remote_upload(NvCache *cache, const int *blocks, int count, size_t half) {
    NvCuda *g = cache->cuda;
    if (cudaSetDevice(g->device) != cudaSuccess)
        return 0;
    float *sides[] = {g->keys, g->values};
    cudaError_t status = cudaSuccess;
    for (int first = 0; first < count && status == cudaSuccess;) {
        int end = first + 1;
        while (end < count && blocks[end] == blocks[end - 1] + 1)
            ++end;
        for (int side = 0; side < 2 && status == cudaSuccess; ++side) {
            void *dst = sides[side] + (size_t)blocks[first] * (half / sizeof(float));
            const unsigned char *src = g->upload_stage + ((size_t)side * count + first) * half;
            status = cudaMemcpyAsync(dst, src, (size_t)(end - first) * half,
                                     cudaMemcpyHostToDevice, g->upload_stream);
        }
        first = end;
    }
    if (status == cudaSuccess)
        status = cudaEventRecord(g->upload_done, g->upload_stream);
    if (status != cudaSuccess)
        (void)cudaStreamSynchronize(g->upload_stream);
    return status == cudaSuccess;
}

int nv_cuda_remote_wait(NvCache *cache) {
    NvCuda *g = cache->cuda;
    cudaError_t status = cudaSetDevice(g->device);
    if (status == cudaSuccess)
        status = cudaEventSynchronize(g->upload_done);
    if (status != cudaSuccess)
        (void)cudaStreamSynchronize(g->upload_stream);
    return status == cudaSuccess;
}
#endif

void nv_cuda_destroy(NvCuda *g) {
    if (!g)
        return;
    check(cudaSetDevice(g->device));
    check(cudaStreamSynchronize(g->stream));
    check(cudaGraphExecDestroy(g->graph));
#ifdef NANO_WITH_STORAGE
    if (g->pool) {
        mi_graph_allocator_destroy(g->workspace);
        mi_mempool_destroy(g->pool);
    } else
#endif
        for (void *buffer : g->buffers)
            check(cudaFree(buffer));
    check(cudaFreeHost(g->host_logits));
    check(cudaFreeHost(g->host_input));
    check(cudaStreamDestroy(g->stream));
    free(g);
}
