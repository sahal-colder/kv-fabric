/*
 * 单机推理接口。声明模型、词表、采样器、页池与调度器类型。
 * 对象由各自的释放 API 回收。调度和本地页索引由同一驱动线程访问。
 */
#ifndef NV_H
#define NV_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* 残差向量维度。 */
    int dim,
    /* 前馈中间层维度。 */
    hidden_dim,
    /* Transformer 层数。 */
    n_layers,
    /* Query 头数。 */
    n_heads,
    /* KV 头数。GQA 中同组 Query 共享一个 KV 头。 */
    n_kv_heads,
    /* 词表大小。 */
    vocab_size,
    /* 最大上下文长度。 */
    seq_len;
} NvConfig;

typedef struct {
    /* token 嵌入表。 */
    float *embedding,
          /* 注意力前的 RMSNorm 权重。 */
          *att_norm,
          /* Query 投影权重。 */
          *wq,
          /* Key 投影权重。 */
          *wk,
          /* Value 投影权重。 */
          *wv,
          /* 注意力输出投影权重。 */
          *wo;
    /* 前馈前的 RMSNorm 权重。 */
    float *ffn_norm,
          /* 前馈门控投影权重。 */
          *w1,
          /* 前馈下投影权重。 */
          *w2,
          /* 前馈上投影权重。 */
          *w3,
          /* 输出分类前的 RMSNorm 权重。 */
          *final_norm,
          /* 词表投影权重，可与 embedding 共享。 */
          *classifier;
} NvWeights;

struct NvCuda;
struct NvSpill;
struct mi_mempool;

typedef struct {
    /* 从检查点读取并校验的模型配置。 */
    NvConfig config;
    /* 指向文件映射的张量视图。 */
    NvWeights weights;
    /* 模型持有的权重文件映射。 */
    void *mapping;
    /* 权重映射字节数。 */
    size_t mapping_size;
    /* 残差流的当前向量。 */
    float *x,
          /* 归一化或注意力输出的工作区。 */
          *xb,
          /* 投影结果的工作区。 */
          *xb2,
          /* 前馈门控向量。 */
          *hb,
          /* 前馈上投影向量。 */
          *hb2,
          /* 当前 token 的 Query 向量。 */
          *q,
          /* 各注意力头的得分和概率工作区。 */
          *att,
          /* 前向计算产生的词表得分。 */
          *logits;
} NvModel;

typedef struct {
    /* 用两种滚动哈希记录前缀指纹。 */
    uint64_t hash, check;
    /* 前缀覆盖的 token 数，0 表示空键。 */
    int length;
} NvPrefixKey;

struct NvPrefixEntry;
typedef struct {
    /* 开放寻址的前缀索引桶。 */
    struct NvPrefixEntry *entries;
    /* 哈希桶下标掩码。 */
    size_t mask;
} NvPrefixIndex;

typedef struct {
    /* 页引用计数，以及空闲前缀 LRU 的前后页号。 */
    int refs, prev, next;
    /* 已发布的完整页前缀键。 */
    NvPrefixKey prefix;
} NvBlock;

typedef struct NvCache {
    /* 物理页数、每页 token 数、层数与每 token 的 KV 向量宽度。 */
    int num_blocks, block_size, layers, kv_dim;
    /* 按物理页号索引的页元数据。 */
    NvBlock *blocks;
    /* 空闲页栈及其长度，随后是可淘汰前缀 LRU 的首尾页号。 */
    int *free_ids, free_count, oldest, newest;
    /* 从前缀键查找物理页的索引。 */
    NvPrefixIndex prefixes;
    /* Key 与 Value 的连续数据区。 */
    float *keys, *values;
    /* 单侧 K 或 V 的浮点元素总数。 */
    size_t elements;
    /* 可选的主机 KV 内存池。 */
    struct mi_mempool *pool;
    /* 可选的 SSD 前缀缓存。 */
    struct NvSpill *spill;
    /* 可选的 CUDA 执行状态。 */
    struct NvCuda *cuda;
    /* 通过前缀复用跳过的 token 数。 */
    uint64_t hits;
    /* 前缀复用开关及 CUDA 设备编号。 */
    int enabled, device;
} NvCache;

typedef struct {
    /* 词条文本，借用词表字符串。 */
    const char *text;
    /* 对应的 token ID。 */
    int id;
} NvTokenIndex;

typedef struct {
    /* 按 token ID 索引的词表字符串。 */
    char **vocab;
    /* 用于选择 BPE 合并顺序的分数。 */
    float *scores;
    /* 按文本排序的二分查找索引。 */
    NvTokenIndex *sorted;
    /* 分词器词表大小。 */
    int vocab_size,
        /* 最长词条的字节长度。 */
        max_length;
    /* 单字节 token 的零结尾输出缓冲区。 */
    char bytes[256][2];
} NvTokenizer;

typedef struct {
    /* 采样温度，0 表示贪心。 */
    float temperature,
          /* 核采样的累计概率阈值。 */
          top_p;
    /* 请求随机数的初始种子。 */
    uint64_t seed;
    /* 忽略结束 token，按生成长度上限继续。 */
    int ignore_eos;
} NvSampling;

typedef enum {
    /* 请求等待调度。 */
    NV_WAITING,
    /* 请求正在执行。 */
    NV_ACTIVE,
    /* 请求已完成生成。 */
    NV_DONE,
    /* 请求已被取消。 */
    NV_CANCELLED
} NvStatus;

typedef struct {
    /* 单调分配的请求 ID。 */
    int id,
        /* 提示词与生成 token 共用的数组。 */
        *tokens,
        /* 提示词的 token 数。 */
        prompt_len,
        /* 当前已有的 token 数。 */
        num_tokens,
        /* 最多再生成多少个 token。 */
        max_new_tokens;
    /* 已完成前向的 token 数。 */
    int computed,
        /* 从序列逻辑页号到物理页号的映射。 */
        *block_table,
        /* 当前持有的物理页引用数。 */
        num_blocks;
    /* 请求的调度状态。 */
    NvStatus status;
    /* 此请求的采样参数。 */
    NvSampling sampling;
    /* 请求私有的随机数状态。 */
    uint64_t rng;
} NvSequence;

typedef struct {
    /* 候选 token 的采样概率。 */
    float prob;
    /* 对应的 token ID。 */
    int id;
} NvCandidate;

typedef struct {
    /* KV 池可用的物理页数。 */
    int num_blocks,
        /* 每个物理页容纳的 token 数。 */
        block_size,
        /* 同时活跃的请求上限。 */
        max_sequences,
        /* 每轮最多前向的 token 数。 */
        token_budget,
        /* 是否启用完整页前缀复用。 */
        prefix_cache,
        /* 请求表总容量。 */
        max_requests,
        /* 是否使用集成构建提供的内存池。 */
        pooled_buffers;
} NvOptions;

typedef struct NvRequestSet {
    /* 第 0 层记录请求 ID，上层每位标记下层一个非空字。 */
    uint64_t *level[6];
    /* 每层位图占用的 64 位字数。 */
    size_t words[6];
    /* 实际分配的位图层数。 */
    unsigned depth;
} NvRequestSet;

typedef struct {
    /* 借用的模型。须晚于引擎释放。 */
    NvModel *model;
    /* 引擎持有的分页 KV 缓存。 */
    NvCache cache;
    /* 引擎使用的容量和调度配置。 */
    NvOptions options;
    /* 按请求 ID 索引的序列表。 */
    NvSequence **requests;
    /* 已登记的请求数和当前活跃数。 */
    int num_requests, active;
    /* 等待与运行请求各自的分层位图。 */
    NvRequestSet waiting, running;
    /* 复用的采样候选缓冲区。 */
    NvCandidate *candidates;
    /* 已执行的调度轮数。 */
    uint64_t steps,
             /* 实际执行前向的 token 数。 */
             forward_tokens,
             /* 已生成的 token 数。 */
             generated_tokens,
             /* 请求被抢占并重新排队的次数。 */
             preemptions;
} NvEngine;

#ifdef NANO_WITH_CUDA

/* 创建引擎并在指定设备初始化 CUDA 前向。 */
void nv_engine_init_cuda(NvEngine *engine, NvModel *model, NvOptions options, int device);
#endif

/* 分配零初始化的数组，失败时结束进程。 */
void *nv_alloc(size_t count, size_t size);

/* 向标准错误输出消息并结束进程。 */
void nv_fatal(const char *message);

/* 就地将得分归一化为概率。 */
void nv_softmax(float *x, int n);

/* 只读映射检查点并初始化模型。 */
void nv_model_load(NvModel *model, const char *path);

/* 使用现成的只读映射初始化模型。映射随后由模型释放。 */
void nv_model_load_mapping(NvModel *model, void *mapping, size_t bytes);

/* 解除模型文件映射并释放前向工作区。 */
void nv_model_free(NvModel *model);

/* 计算一个 token，返回借用的词表 logits 缓冲区。 */
float *nv_forward(NvModel *model, NvCache *cache, const int *table, int token, int pos);

/* 读取词表、合并分数和查词索引。 */
void nv_tokenizer_load(NvTokenizer *tokenizer, const char *path, int vocab_size);

/* 释放分词器持有的词表和索引。 */
void nv_tokenizer_free(NvTokenizer *tokenizer);

/* 编码文本并返回 token 数组。调用方负责 free。 */
int *nv_encode(const NvTokenizer *tokenizer, const char *text, int *count);

/* 返回 token 的文本视图，由分词器持有。 */
const char *nv_decode(const NvTokenizer *tokenizer, int previous, int token);

/* 将可显示的文本片段写入输出流。 */
void nv_print_piece(FILE *file, const char *piece);

/* 创建分页 KV 元数据和连续 K/V 数据区。 */
void nv_cache_init(NvCache *cache, const NvConfig *config, NvOptions options);

/* 释放 KV 数据区、索引和可选后端。 */
void nv_cache_free(NvCache *cache);

/* 取得空闲页或淘汰空闲前缀页。无可用页时返回 -1。 */
int nv_cache_allocate(NvCache *cache);

/* 归还序列的页引用，并重置已计算进度。 */
void nv_cache_release(NvCache *cache, NvSequence *seq);

/* 为序列匹配可复用的完整前缀页。 */
void nv_cache_reuse(NvCache *cache, NvSequence *seq);

/* 将刚计算完的完整页发布到前缀索引。 */
void nv_cache_publish(NvCache *cache, NvSequence *seq);

/* 返回物理页内指定层与位置的 Key 地址。 */
float *nv_cache_key(NvCache *cache, int block, int layer, int offset);
/* 返回物理页内指定层与位置的 Value 地址。 */
float *nv_cache_value(NvCache *cache, int block, int layer, int offset);

/* 创建使用 CPU 前向的推理引擎。 */
void nv_engine_init(NvEngine *engine, NvModel *model, NvOptions options);

/* 释放引擎持有的请求、KV 和采样区。 */
void nv_engine_free(NvEngine *engine);

/* 复制输入 token 并返回请求 ID。容量或参数无效时返回 -1。 */
int nv_engine_add(NvEngine *engine, const int *tokens, int count, int max_new, NvSampling sampling);

/* 取消尚未完成的请求，成功返回 1。 */
int nv_engine_cancel(NvEngine *engine, int id);

/* 执行一轮调度，返回本轮前向的 token 数。 */
int nv_engine_step(NvEngine *engine);

/* 从 logits 选出下一个 token ID。 */
int nv_sample(float *logits, int count, NvSampling options, uint64_t *rng, NvCandidate *scratch);

#ifdef __cplusplus
}
#endif

#endif
