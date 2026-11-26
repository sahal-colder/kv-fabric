#ifndef KVFABRIC_VLLM_BRIDGE_H
#define KVFABRIC_VLLM_BRIDGE_H

#include "kvfabric/kvfabric.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kvf_vllm_worker kvf_vllm_worker;

typedef struct {
    const char *control;
    const char *model_key;
    const char *tenant;
    uint32_t block_tokens, layers, layer_bytes;
} kvf_vllm_spec;

/* Hashes are the ordered full-page chain. The first page has a zero parent. */
kvf_status kvf_vllm_hashes(const kvf_vllm_spec *spec, const int32_t *tokens,
                          size_t pages, kvf_hash *out);
kvf_status kvf_vllm_lookup(const kvf_vllm_spec *spec, const kvf_hash *hashes,
                          size_t pages, uint32_t timeout_ms, size_t *matched);
kvf_status kvf_vllm_open(const kvf_vllm_spec *spec, const char *name,
                        const char *disk_path, size_t cache_pages,
                        uint32_t io_depth, kvf_vllm_worker **out);
const char *kvf_vllm_io_mode(const kvf_vllm_worker *worker);
void kvf_vllm_close(kvf_vllm_worker *worker);

/* Payload order is [page][layer][K or V][token][head][head_dim], BF16.
 * Wrap calls with the Host buffer's full lifetime and bound calls to a window. */
kvf_status kvf_vllm_publish(kvf_vllm_worker *worker, const kvf_hash *parent,
                           const int32_t *tokens, size_t pages, const void *data);
kvf_status kvf_vllm_load(kvf_vllm_worker *worker, const kvf_hash *parent,
                        const int32_t *tokens, size_t pages, uint32_t timeout_ms,
                        void *data);

/* Copy the longest validated contiguous run owned by this Worker. The caller
 * handles the first missing page and remaining suffix via the normal fetch. */
size_t kvf_vllm_load_owned(kvf_vllm_worker *worker, const kvf_hash *hashes,
                          size_t pages, uint32_t timeout_ms, void *data);

#ifdef __cplusplus
}
#endif

#endif
