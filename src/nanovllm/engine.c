/*
 * 推理调度层。推进请求状态、分页前缀和每步模型计算。
 * 引擎拥有活动请求状态并借用模型。调度由同一驱动线程串行推进。
 */
#define _POSIX_C_SOURCE 200809L
#include "nano_cache.h"
#ifdef NANO_WITH_REMOTE_KV
#include "nano_remote_kv.h"
#endif
#ifdef NANO_WITH_CUDA
#include "nano_cuda.h"
#endif
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 每层按 64 位一组汇总下层，直至顶层只剩一个字。 */
static void request_set_init(NvRequestSet *set, size_t requests) {
    size_t words = (requests + 63) / 64;
    for (unsigned level = 0;; ++level) {
        set->words[level] = words;
        set->level[level] = nv_alloc(words, sizeof(uint64_t));
        ++set->depth;
        if (words == 1)
            return;
        words = (words + 63) / 64;
    }
}

/* Each summary bit records whether its child word is nonempty. */
static void request_set_update(NvRequestSet *set, int id, int insert) {
    size_t bit = (size_t)id;
    for (unsigned level = 0; level < set->depth; ++level) {
        uint64_t *word = &set->level[level][bit / 64];
        uint64_t mask = UINT64_C(1) << (bit % 64);
        uint64_t old = *word;
        *word = insert ? old | mask : old & ~mask;
        if (!!old == !!*word)
            break;
        /* 上层的一位对应下层的一整个字。 */
        bit /= 64;
    }
}

static int request_at_or_after(const NvRequestSet *set, unsigned level, size_t bit) {
    size_t word = bit / 64;
    if (word >= set->words[level])
        return -1;
    uint64_t bits = set->level[level][word] & (UINT64_MAX << (bit % 64));
    if (!bits && level + 1 < set->depth) {
        /* 用摘要跳过下层空字，定位下一组候选请求。 */
        int next = request_at_or_after(set, level + 1, word + 1);
        if (next < 0)
            return -1;
        word = (size_t)next;
        bits = set->level[level][word];
    }
    return bits ? (int)(word * 64 + __builtin_ctzll(bits)) : -1;
}

/* 从最高置位逐层定位最晚的活跃请求。 */
static int last_request(const NvRequestSet *set) {
    size_t word = 0;
    for (unsigned level = set->depth; level--;) {
        uint64_t bits = set->level[level][word];
        if (!bits)
            return -1;
        word = word * 64 + 63 - __builtin_clzll(bits);
    }
    return (int)word;
}

static void init_engine(NvEngine *e, NvModel *m, NvOptions o) {
    if (o.num_blocks <= 0 || o.block_size <= 0 || o.block_size > m->config.seq_len ||
        o.max_sequences <= 0 || o.token_budget <= 0 || o.max_requests < 0)
        nv_fatal("invalid engine options");
    if (!o.max_requests)
        o.max_requests = o.max_sequences > 4096 ? o.max_sequences : 4096;
    *e = (NvEngine){.model = m, .options = o};
    e->requests = nv_alloc((size_t)o.max_requests, sizeof(*e->requests));
    e->candidates = nv_alloc((size_t)m->config.vocab_size, sizeof(*e->candidates));
    request_set_init(&e->waiting, (size_t)o.max_requests);
    request_set_init(&e->running, (size_t)o.max_requests);
#ifdef NANO_WITH_REMOTE_KV
    request_set_init(&e->remote_pending, (size_t)o.max_requests);
#endif
}

void nv_engine_init(NvEngine *e, NvModel *m, NvOptions o) {
    init_engine(e, m, o);
    nv_cache_init(&e->cache, &m->config, o);
}

#ifdef NANO_WITH_CUDA
void nv_engine_init_cuda(NvEngine *e, NvModel *m, NvOptions o, int device) {
    init_engine(e, m, o);
    nv_cache_init_metadata(&e->cache, &m->config, o);
    nv_cuda_init(e, device);
}
#endif

static void free_requests(NvEngine *e) {
    for (int i = 0; i < e->num_requests; ++i) {
        free(e->requests[i]->tokens);
        free(e->requests[i]->block_table);
        free(e->requests[i]);
        e->requests[i] = NULL;
    }
    e->num_requests = 0;
}

int nv_engine_clear_requests(NvEngine *e) {
    if (nv_engine_has_work(e))
        return 0;
    free_requests(e);
    return 1;
}

void nv_engine_free(NvEngine *e) {
#ifdef NANO_WITH_REMOTE_KV
    if (e->remote)
        nv_remote_close(e);
#endif
    free_requests(e);
    for (unsigned i = 0; i < e->waiting.depth; ++i) {
        free(e->waiting.level[i]);
        free(e->running.level[i]);
        free(e->remote_pending.level[i]);
    }
    free(e->requests);
    free(e->candidates);
    nv_cache_free(&e->cache);
    *e = (NvEngine){0};
}

/* 校验上下文与页池容量后，将请求加入等待位图。 */
int nv_engine_add(NvEngine *e, const int *tokens, int n, int max_new, NvSampling sampling) {
    int context = e->model->config.seq_len;
    if (!tokens || n < 1 || n > context || max_new < 1 || max_new > context - n + 1 ||
        n > INT_MAX - max_new || e->num_requests == e->options.max_requests ||
        !isfinite(sampling.temperature) || sampling.temperature < 0 ||
        !isfinite(sampling.top_p) || sampling.top_p <= 0 || sampling.top_p > 1)
        return -1;
    /* 最后生成的 token 只输出，无须为它预留前向页。 */
    size_t forwards = (size_t)n + (size_t)max_new - 1;
    size_t blocks = (forwards - 1) / (size_t)e->options.block_size + 1;
    if (blocks > (size_t)e->options.num_blocks)
        return -1;
    for (int i = 0; i < n; ++i)
        if ((unsigned)tokens[i] >= (unsigned)e->model->config.vocab_size)
            return -1;

    NvSequence *s = nv_alloc(1, sizeof(*s));
    *s = (NvSequence){.id = e->num_requests, .prompt_len = n, .num_tokens = n,
        .max_new_tokens = max_new, .sampling = sampling, .rng = sampling.seed ? sampling.seed : 1};
    s->tokens = nv_alloc((size_t)n + max_new, sizeof(*s->tokens));
    memcpy(s->tokens, tokens, (size_t)n * sizeof(*s->tokens));
    s->block_table = nv_alloc(blocks, sizeof(*s->block_table));
    e->requests[e->num_requests++] = s;
    request_set_update(&e->waiting, s->id, 1);
    return s->id;
}

/* 从调度位图移除请求并释放已持有的页。 */
int nv_engine_cancel(NvEngine *e, int id) {
    if ((unsigned)id >= (unsigned)e->num_requests)
        return 0;
    NvSequence *s = e->requests[id];
    if (s->status == NV_DONE || s->status == NV_CANCELLED)
        return 0;
    if (s->status == NV_ACTIVE) {
        request_set_update(&e->running, id, 0);
        nv_cache_release(&e->cache, s);
        --e->active;
    }
#ifdef NANO_WITH_REMOTE_KV
    else if (s->status == NV_REMOTE_PREFETCHING) {
        request_set_update(&e->remote_pending, id, 0);
        nv_remote_cancel(e, s);
        nv_cache_release(&e->cache, s);
        --e->active;
    }
#endif
    else {
        request_set_update(&e->waiting, id, 0);
    }
    s->status = NV_CANCELLED;
    return 1;
}

static int allocate_block(NvEngine *e, const NvSequence *s) {
    int block = nv_cache_allocate(&e->cache);
    while (block < 0) {
        int id = last_request(&e->running);
#ifdef NANO_WITH_REMOTE_KV
        if (e->remote) {
            int pending = last_request(&e->remote_pending);
            if (pending > id)
                id = pending;
        }
#endif
        /* 只抢占排在当前请求之后的请求。 */
        if (id <= s->id)
            break;
        NvSequence *victim = e->requests[id];
#ifdef NANO_WITH_REMOTE_KV
        if (victim->status == NV_REMOTE_PREFETCHING) {
            request_set_update(&e->remote_pending, id, 0);
            nv_remote_cancel(e, victim);
        } else
#endif
            request_set_update(&e->running, id, 0);
        request_set_update(&e->waiting, id, 1);
        /* 归还被抢占请求的页。重新调度时可命中前缀。 */
        nv_cache_release(&e->cache, victim);
        victim->status = NV_WAITING;
        --e->active;
        ++e->preemptions;
        block = nv_cache_allocate(&e->cache);
    }
    return block;
}

int nv_engine_has_work(const NvEngine *e) {
    return e->active || last_request(&e->waiting) >= 0;
}

#ifdef NANO_WITH_REMOTE_KV
static void collect_remote(NvEngine *e) {
    nv_remote_reap(e->remote);
    size_t cursor = 0;
    for (;;) {
        int id = request_at_or_after(&e->remote_pending, 0, cursor);
        if (id < 0)
            return;
        cursor = (size_t)id + 1;
        NvSequence *s = e->requests[id];
        if (nv_remote_poll(e, s)) {
            request_set_update(&e->remote_pending, id, 0);
            request_set_update(&e->running, id, 1);
            s->status = NV_ACTIVE;
        }
    }
}
#endif

/* 在本轮 token 预算内推进等待和活跃请求。 */
int nv_engine_step(NvEngine *e) {
#ifdef NANO_WITH_REMOTE_KV
    if (e->remote)
        collect_remote(e);
#endif
    int quota = e->options.token_budget / e->options.max_sequences;
    if (!quota)
        quota = 1;
    int used = 0;
    size_t cursor = 0;
    for (int scheduled = 0; scheduled < e->options.max_sequences && used < e->options.token_budget;
         ++scheduled) {
        int running = request_at_or_after(&e->running, 0, cursor);
        int waiting = e->active < e->options.max_sequences
            ? request_at_or_after(&e->waiting, 0, cursor) : -1;
        int id = waiting >= 0 && (running < 0 || waiting < running) ? waiting : running;
        if (id < 0)
            break;
        /* 本轮的下一次选择从当前 ID 之后开始。 */
        cursor = (size_t)id + 1;

        NvSequence *s = e->requests[id];
        if (s->status == NV_WAITING) {
            nv_cache_reuse(&e->cache, s);
            request_set_update(&e->waiting, id, 0);
            ++e->active;
#ifdef NANO_WITH_REMOTE_KV
            if (e->remote && nv_remote_begin(e, s)) {
                s->status = NV_REMOTE_PREFETCHING;
                request_set_update(&e->remote_pending, id, 1);
                continue;
            }
#endif
            request_set_update(&e->running, id, 1);
            s->status = NV_ACTIVE;
        }

        for (int i = 0; i < quota && used < e->options.token_budget; ++i) {
            /* 跨入新逻辑页时先分配物理页。 */
            if (s->computed / e->options.block_size == s->num_blocks) {
                int block = allocate_block(e, s);
                if (block < 0)
                    break;
                s->block_table[s->num_blocks++] = block;
            }
            float *logits = nv_forward(e->model, &e->cache, s->block_table, s->tokens[s->computed],
                                       s->computed);
            ++s->computed;
            ++used;
            ++e->forward_tokens;
            if (s->computed == s->prompt_len && !s->prefill_done_ns) {
                struct timespec time;
                if (clock_gettime(CLOCK_MONOTONIC, &time) == 0)
                    s->prefill_done_ns = (uint64_t)time.tv_sec * UINT64_C(1000000000) +
                                         (uint64_t)time.tv_nsec;
            }
            nv_cache_publish(&e->cache, s);
            if (s->computed <= s->prompt_len) {
                ++e->prefill_tokens;
#ifdef NANO_WITH_REMOTE_KV
                if (e->remote && !(s->computed % e->options.block_size))
                    nv_remote_publish(e, s);
#endif
            }
            /* 先算完已有 token，再根据最后一个位置的 logits 采样。 */
            if (s->computed < s->num_tokens)
                continue;

            int token = nv_sample(logits, e->model->config.vocab_size, s->sampling,
                                  &s->rng, e->candidates);
            s->tokens[s->num_tokens++] = token;
            ++e->generated_tokens;
            if (s->num_tokens - s->prompt_len == s->max_new_tokens ||
                (!s->sampling.ignore_eos && (token == 1 || token == 2))) {
                s->status = NV_DONE;
                request_set_update(&e->running, id, 0);
                nv_cache_release(&e->cache, s);
                --e->active;
            }
            break;
        }
    }
    ++e->steps;
    assert(used || !nv_engine_has_work(e) ||
           (e->remote && last_request(&e->remote_pending) >= 0));
    return used;
}
