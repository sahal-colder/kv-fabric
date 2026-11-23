/*
 * 推理采样层。根据 logits 与随机状态选择下一个 token。
 * 采样器拥有其概率工作区，由所属请求串行访问。
 */
#include "nv.h"
#include <math.h>
#include <stdlib.h>

/* 按概率降序排列候选。 */
static int compare(const void *a, const void *b) {
    const NvCandidate *x = a, *y = b;
    if (x->prob != y->prob) return x->prob > y->prob ? -1 : 1;
    return (x->id > y->id) - (x->id < y->id);
}

/* 温度为 0 时取最大得分，否则执行概率采样。 */
int nv_sample(float *logits, int n, NvSampling o, uint64_t *rng, NvCandidate *scratch) {
    if (o.temperature == 0) {
        int best = 0;

        for (int i = 1; i < n; i++) if (logits[i] > logits[best]) best = i;
        return best;
    }

    float max = logits[0];
    for (int i = 1; i < n; i++) if (logits[i] > max) max = logits[i];
    for (int i = 0; i < n; i++) logits[i] = (logits[i] - max) / o.temperature;
    nv_softmax(logits, n);

    for (int i = 0; i < n; i++) scratch[i] = (NvCandidate){logits[i], i};
    qsort(scratch, (size_t)n, sizeof(*scratch), compare);

    int keep = 0;
    float mass = 0;
    do { mass += scratch[keep++].prob; } while (keep < n && mass < o.top_p);

    *rng ^= *rng >> 12;
    *rng ^= *rng << 25;
    *rng ^= *rng >> 27;
    float coin = (float)(((*rng * UINT64_C(2685821657736338717)) >> 40)) / 16777216.0f;

    float target = coin * mass;
    for (int i = 0; i < keep; i++) {
        target -= scratch[i].prob;
        if (target < 0) return scratch[i].id;
    }

    return scratch[keep - 1].id;
}
