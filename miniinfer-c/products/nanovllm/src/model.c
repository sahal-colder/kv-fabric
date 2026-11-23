/*
 * 单机模型层。装入权重并执行模型前向计算。
 * 模型拥有权重映射，推理请求持有工作区。只读权重无锁。
 */
#define _POSIX_C_SOURCE 200809L
#include "nv.h"
#ifdef NANO_WITH_CUDA
#include "nano_cuda.h"
#endif
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef NV_USE_OPENBLAS
#include <cblas.h>
#endif

void nv_fatal(const char *message) {
    fprintf(stderr, "nanovllm: %s\n", message);
    exit(EXIT_FAILURE);
}

void *nv_alloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) nv_fatal("allocation size overflow");
    void *p = calloc(count ? count : 1, size);
    if (!p) nv_fatal("out of memory");
    return p;
}

/* 按检查点布局取张量视图，并推进文件偏移。 */
static float *tensor(NvModel *m, size_t *offset, size_t a, size_t b, size_t c) {
    size_t available = (m->mapping_size - *offset) / sizeof(float);
    if (a > available / b / c) nv_fatal("truncated checkpoint or invalid dimensions");
    float *p = (float *)((char *)m->mapping + *offset);
    *offset += a * b * c * sizeof(float);
    return p;
}

void nv_model_load(NvModel *m, const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        nv_fatal("cannot open checkpoint");
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < (off_t)sizeof(NvConfig) || (uintmax_t)st.st_size > SIZE_MAX) {
        close(fd);
        nv_fatal("invalid checkpoint size");
    }
    size_t bytes = (size_t)st.st_size;
    void *mapping = mmap(NULL, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED)
        nv_fatal("cannot map checkpoint");
    nv_model_load_mapping(m, mapping, bytes);
}

void nv_model_load_mapping(NvModel *m, void *mapping, size_t bytes) {
    *m = (NvModel){0};

    const uint32_t endian = 1;
    if (*(const unsigned char *)&endian != 1 || sizeof(float) != 4 || sizeof(int) != 4)
        nv_fatal("requires little-endian 32-bit int/float host");
    if (!mapping || mapping == MAP_FAILED || bytes < sizeof(NvConfig))
        nv_fatal("invalid checkpoint mapping");
    m->mapping = mapping;
    m->mapping_size = bytes;

    /* 文件头有七个 32 位配置字段。 */
    memcpy(&m->config, m->mapping, 28);
    NvConfig *p = &m->config;

    if (p->dim <= 0 || p->hidden_dim <= 0 || p->n_layers <= 0 || p->n_heads <= 0 ||
        p->n_kv_heads <= 0 || p->seq_len <= 0 || p->vocab_size == INT_MIN ||
        abs(p->vocab_size) < 259 || p->dim % p->n_heads ||
        (p->dim / p->n_heads) % 2 || p->n_heads % p->n_kv_heads)
        nv_fatal("invalid model config (expected llama2.c legacy FP32 format)");

    /* 词表大小的符号标记分类权重是否与嵌入共享。 */
    int shared = p->vocab_size > 0;
    p->vocab_size = abs(p->vocab_size);

    size_t d = (size_t)p->dim, h = (size_t)p->hidden_dim, l = (size_t)p->n_layers;
    size_t kv = (d / (size_t)p->n_heads) * (size_t)p->n_kv_heads;
    size_t offset = 28;
    NvWeights *w = &m->weights;

    w->embedding = tensor(m, &offset, (size_t)p->vocab_size, d, 1);
    w->att_norm = tensor(m, &offset, l, d, 1);
    w->wq = tensor(m, &offset, l, d, d);
    w->wk = tensor(m, &offset, l, kv, d);
    w->wv = tensor(m, &offset, l, kv, d);
    w->wo = tensor(m, &offset, l, d, d);
    w->ffn_norm = tensor(m, &offset, l, d, 1);
    w->w1 = tensor(m, &offset, l, h, d);
    w->w2 = tensor(m, &offset, l, d, h);
    w->w3 = tensor(m, &offset, l, h, d);
    w->final_norm = tensor(m, &offset, d, 1, 1);

    /* 跳过检查点内的 RoPE 表，前向时重新计算旋转角。 */
    (void)tensor(m, &offset, (size_t)p->seq_len, d / (size_t)p->n_heads, 1);

    w->classifier = shared ? w->embedding : tensor(m, &offset, (size_t)p->vocab_size, d, 1);

    m->x = nv_alloc(d, sizeof(float));
    m->xb = nv_alloc(d, sizeof(float));
    m->xb2 = nv_alloc(d, sizeof(float));
    m->q = nv_alloc(d, sizeof(float));
    m->hb = nv_alloc(h, sizeof(float));
    m->hb2 = nv_alloc(h, sizeof(float));
    m->att = nv_alloc((size_t)p->n_heads * (size_t)p->seq_len, sizeof(float));
    m->logits = nv_alloc((size_t)p->vocab_size, sizeof(float));
}

void nv_model_free(NvModel *m) {
    if (m->mapping_size)
        (void)munmap(m->mapping, m->mapping_size);
    free(m->x); free(m->xb); free(m->xb2); free(m->q);
    free(m->hb); free(m->hb2); free(m->att); free(m->logits);

    *m = (NvModel){0};
}

static void rmsnorm(float *out, const float *x, const float *weight, int n) {
    float sum = 0;
    for (int i = 0; i < n; i++) sum += x[i] * x[i];
    /* 小常数避免平方均值接近零时除零。 */
    float scale = 1.0f / sqrtf(sum / (float)n + 1e-5f);
    for (int i = 0; i < n; i++) out[i] = weight[i] * (scale * x[i]);
}

void nv_softmax(float *x, int n) {
    float max = x[0];
    for (int i = 1; i < n; i++) if (x[i] > max) max = x[i];
    float sum = 0;
    /* 先减最大得分，再取指数以避免溢出。 */
    for (int i = 0; i < n; i++) { x[i] = expf(x[i] - max); sum += x[i]; }
    for (int i = 0; i < n; i++) x[i] /= sum;
}

static void matmul(float *out, const float *x, const float *w, int in, int rows) {
#ifdef NV_USE_OPENBLAS
    cblas_sgemv(CblasRowMajor, CblasNoTrans, rows, in, 1.0f, w, in, x, 1, 0.0f, out, 1);
#else
#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (int i = 0; i < rows; i++) {
        float sum = 0;
        for (int j = 0; j < in; j++) sum += w[(size_t)i * (size_t)in + (size_t)j] * x[j];
        out[i] = sum;
    }
#endif
}

float *nv_forward(NvModel *m, NvCache *cache, const int *table, int token, int pos) {
#ifdef NANO_WITH_CUDA
    if (cache->cuda)
        return nv_cuda_forward(cache, table, token, pos);
#endif
    NvConfig *p = &m->config;
    NvWeights *w = &m->weights;
    int d = p->dim, h = p->hidden_dim, hs = d / p->n_heads;
    int kv = hs * p->n_kv_heads, group = p->n_heads / p->n_kv_heads;

    size_t dd = (size_t)d * (size_t)d, kd = (size_t)kv * (size_t)d;
    size_t hd = (size_t)h * (size_t)d;

    memcpy(m->x, w->embedding + (size_t)token * (size_t)d, (size_t)d * sizeof(float));
    for (int layer = 0; layer < p->n_layers; layer++) {
        size_t l = (size_t)layer;

        rmsnorm(m->xb, m->x, w->att_norm + l * (size_t)d, d);

        /* 将当前 token 的 Key 投影直接写入对应的物理页。 */
        float *k = nv_cache_key(cache, table[pos / cache->block_size], layer, pos % cache->block_size);
        float *v = nv_cache_value(cache, table[pos / cache->block_size], layer, pos % cache->block_size);
        matmul(m->q, m->xb, w->wq + l * dd, d, d);
        matmul(k, m->xb, w->wk + l * kd, d, kv);
        matmul(v, m->xb, w->wv + l * kd, d, kv);

        for (int i = 0; i < d; i += 2) {
            /* 按绝对位置和头内维度计算 RoPE 旋转角。 */
            float angle = (float)pos / powf(10000.0f, (float)(i % hs) / (float)hs);
            float cs = cosf(angle), sn = sinf(angle), q0 = m->q[i], q1 = m->q[i + 1];
            m->q[i] = q0 * cs - q1 * sn;
            m->q[i + 1] = q0 * sn + q1 * cs;
            /* GQA 的 Key 总宽度可能小于 Query，只旋转实际存在的分量。 */
            if (i < kv) {
                float k0 = k[i], k1 = k[i + 1];
                k[i] = k0 * cs - k1 * sn;
                k[i + 1] = k0 * sn + k1 * cs;
            }
        }

#ifdef _OPENMP
#pragma omp parallel for
#endif
        for (int head = 0; head < p->n_heads; head++) {
            float *q = m->q + head * hs;

            float *att = m->att + (size_t)head * (size_t)p->seq_len;

            for (int t = 0; t <= pos; t++) {
                const float *kt = nv_cache_key(cache, table[t / cache->block_size], layer,
                                               t % cache->block_size) + (head / group) * hs;
                float score = 0;
                for (int j = 0; j < hs; j++) score += q[j] * kt[j];

                att[t] = score / sqrtf((float)hs);
            }
            /* 只对当前及过去位置归一化，保持因果注意力。 */
            nv_softmax(att, pos + 1);

            float *out = m->xb + head * hs;
            memset(out, 0, (size_t)hs * sizeof(float));

            for (int t = 0; t <= pos; t++) {
                const float *vt = nv_cache_value(cache, table[t / cache->block_size], layer,
                                                 t % cache->block_size) + (head / group) * hs;
                for (int j = 0; j < hs; j++) out[j] += att[t] * vt[j];
            }
        }

        matmul(m->xb2, m->xb, w->wo + l * dd, d, d);
        for (int i = 0; i < d; i++) m->x[i] += m->xb2[i];

        rmsnorm(m->xb, m->x, w->ffn_norm + l * (size_t)d, d);
        matmul(m->hb, m->xb, w->w1 + l * hd, d, h);
        matmul(m->hb2, m->xb, w->w3 + l * hd, d, h);

        /* SwiGLU 对门控支路应用 SiLU，再与上投影逐项相乘。 */
        for (int i = 0; i < h; i++) m->hb[i] = (m->hb[i] * (1.0f / (1.0f + expf(-m->hb[i])))) * m->hb2[i];

        matmul(m->xb, m->hb, w->w2 + l * hd, h, d);
        for (int i = 0; i < d; i++) m->x[i] += m->xb[i];
    }

    rmsnorm(m->x, m->x, w->final_norm, d);

    matmul(m->logits, m->x, w->classifier, d, p->vocab_size);

    return m->logits;
}
