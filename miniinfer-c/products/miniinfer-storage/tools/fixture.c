/*
 * FP32 test fixture and scalar Attention oracle. This layer owns temporary
 * allocations and files, holds no locks, and does not call any device API.
 */
#include "fixture.h"
#include "kvfabric/wire.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const int32_t kvf_fixture_tokens[] = {91, 92, 93, 94, 95, 96, 97, 98};

int kvf_fixture_identity(size_t bytes, kvf_identity *id) {
    const uint32_t little_endian = 1;
    if (sizeof(float) != 4 || FLT_RADIX != 2 || FLT_MANT_DIG != 24 ||
        *(const uint8_t *)&little_endian != 1 || !bytes ||
        bytes > KVF_MAX_LAYER_BYTES || bytes % (2 * TOKENS_PER_PAGE * sizeof(float)))
        return 0;
    *id = (kvf_identity){
        .tokens_per_page = TOKENS_PER_PAGE, .layers = LAYERS, .bytes_per_layer = (uint32_t)bytes,
        .dtype = KVF_F32, .tp_size = 1, .pp_size = 1
    };
    return kvf_hash_bytes("fixture-model-v1", 16, &id->model) == KVF_OK &&
        kvf_hash_bytes("fixture-tokenizer-v1", 20, &id->tokenizer) == KVF_OK &&
        kvf_hash_bytes("le-fp32-k-then-v", 16, &id->layout) == KVF_OK &&
        kvf_hash_bytes("fixture-tenant", 14, &id->tenant) == KVF_OK;
}

void kvf_fixture_fill(float *out, size_t count, size_t first) {
    for (size_t i = 0; i < count; ++i) {
        size_t index = i + first;
        out[i] = (float)((int)((index * 61 + index / 259) % 257) - 128) / 4096.0f;
    }
}

static FILE *open_in(const char *dir, const char *name, const char *mode) {
    char path[1024];
    int len = snprintf(path, sizeof(path), "%s/%s", dir, name);
    return len > 0 && (size_t)len < sizeof(path) ? fopen(path, mode) : NULL;
}

int kvf_fixture_write(const char *dir, size_t bytes) {
    kvf_identity id;
    if (!kvf_fixture_identity(bytes, &id))
        return 0;
    FILE *ns = open_in(dir, "namespace", "w");
    FILE *tokens = open_in(dir, "tokens", "wb");
    FILE *data = open_in(dir, "tensors", "wb");
    size_t total = PAGES * LAYERS * bytes;
    float *values = malloc(total);
    int ok = ns && tokens && data && values;
    if (ok) {
        kvf_hash identities[] = {id.model, id.tokenizer, id.layout, id.lora, id.tenant};
        char hex[65];
        for (size_t i = 0; ok && i < sizeof(identities) / sizeof(*identities); ++i) {
            kvf_hash_format(identities[i], hex);
            ok = fprintf(ns, "%s\n", hex) > 0;
        }
        ok = ok && fprintf(ns, "%d %d %zu 1 0 1 0 1\n", TOKENS_PER_PAGE, LAYERS, bytes) > 0;
        uint8_t wire[4 * PAGES * TOKENS_PER_PAGE];
        kvf_writer w = {.buffer = wire, .capacity = sizeof(wire)};
        for (unsigned i = 0; i < PAGES * TOKENS_PER_PAGE; ++i)
            kvf_put_u32(&w, (uint32_t)kvf_fixture_tokens[i]);
        kvf_fixture_fill(values, total / sizeof(*values), 0);
        ok = ok && !w.failed && fwrite(wire, 1, sizeof(wire), tokens) == sizeof(wire) &&
            fwrite(values, 1, total, data) == total;
    }
    free(values);
    if (data)
        ok = fclose(data) == 0 && ok;
    if (tokens)
        ok = fclose(tokens) == 0 && ok;
    if (ns)
        ok = fclose(ns) == 0 && ok;
    return ok;
}

/* One attention head over both pages. Compare reference and imported tensors
 * on the same host so libm rounding differences between platforms are irrelevant. */
static void attention(const float *data, size_t layer_floats, size_t layer, float *out) {
    size_t dim = layer_floats / (2 * TOKENS_PER_PAGE);
    float scores[PAGES * TOKENS_PER_PAGE], high = -INFINITY;
    float scale = 1.0f / sqrtf((float)dim);
    for (size_t pos = 0; pos < PAGES * TOKENS_PER_PAGE; ++pos) {
        size_t page = pos / TOKENS_PER_PAGE, offset = pos % TOKENS_PER_PAGE;
        const float *key = data + (page * LAYERS + layer) * layer_floats + offset * dim;
        float dot = 0.0f;
        for (size_t j = 0; j < dim; ++j)
            dot += ((float)((int)(j % 23) - 11) / 128.0f) * key[j];
        scores[pos] = dot * scale;
        high = fmaxf(high, scores[pos]);
    }
    float denominator = 0.0f;
    for (size_t pos = 0; pos < PAGES * TOKENS_PER_PAGE; ++pos) {
        scores[pos] = expf(scores[pos] - high);
        denominator += scores[pos];
    }
    memset(out, 0, dim * sizeof(*out));
    for (size_t pos = 0; pos < PAGES * TOKENS_PER_PAGE; ++pos) {
        size_t page = pos / TOKENS_PER_PAGE, offset = pos % TOKENS_PER_PAGE;
        const float *value = data + (page * LAYERS + layer) * layer_floats +
            (TOKENS_PER_PAGE + offset) * dim;
        float weight = scores[pos] / denominator;
        for (size_t j = 0; j < dim; ++j)
            out[j] += weight * value[j];
    }
}

int kvf_fixture_check(size_t bytes, const char *path, kvf_hash *sha) {
    kvf_identity id;
    if (!kvf_fixture_identity(bytes, &id))
        return 0;
    size_t total = PAGES * LAYERS * bytes, output_bytes = bytes / (2 * TOKENS_PER_PAGE);
    float *reference = malloc(total), *actual = malloc(total);
    float *want = malloc(output_bytes), *have = malloc(output_bytes);
    FILE *file = fopen(path, "rb");
    int ok = file && reference && actual && want && have;
    if (ok) {
        kvf_fixture_fill(reference, total / sizeof(*reference), 0);
        kvf_hash expected;
        ok = fread(actual, 1, total, file) == total && fgetc(file) == EOF && !ferror(file) &&
            !memcmp(reference, actual, total) && kvf_hash_bytes(reference, total, &expected) == KVF_OK &&
            kvf_hash_bytes(actual, total, sha) == KVF_OK && kvf_hash_equal(expected, *sha);
    }
    for (size_t layer = 0; ok && layer < LAYERS; ++layer) {
        attention(reference, bytes / sizeof(float), layer, want);
        attention(actual, bytes / sizeof(float), layer, have);
        ok = !memcmp(want, have, output_bytes);
    }
    if (file)
        ok = fclose(file) == 0 && ok;
    free(have);
    free(want);
    free(actual);
    free(reference);
    return ok;
}
