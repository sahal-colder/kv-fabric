/* Deterministic FP32 tensor fixture for the demo, E2E test and benchmark. No locks or shared ownership. */
#ifndef KVF_FIXTURE_H
#define KVF_FIXTURE_H
#include "kvfabric/kvfabric.h"

enum { PAGES = 2, LAYERS = 2, TOKENS_PER_PAGE = 4 };
extern const int32_t kvf_fixture_tokens[PAGES * TOKENS_PER_PAGE];
int kvf_fixture_identity(size_t bytes, kvf_identity *id);
void kvf_fixture_fill(float *out, size_t count, size_t first);
int kvf_fixture_write(const char *dir, size_t bytes);
int kvf_fixture_check(size_t bytes, const char *path, kvf_hash *sha);

#endif
