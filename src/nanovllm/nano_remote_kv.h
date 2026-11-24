#ifndef NANO_REMOTE_KV_H
#define NANO_REMOTE_KV_H

#include "nv.h"

/* All entry points are driven by the Nano-VLLM scheduling thread. The
 * publication thread consumes owned snapshots and never reads Nano pages. */
int nv_remote_begin(NvEngine *engine, NvSequence *sequence);
int nv_remote_poll(NvEngine *engine, NvSequence *sequence);
void nv_remote_cancel(NvEngine *engine, NvSequence *sequence);
void nv_remote_reap(NvEngine *engine);
void nv_remote_detach_sequences(struct NvRemoteKV *remote);
void nv_remote_note_forward(struct NvRemoteKV *remote, uint64_t elapsed);
int nv_remote_publish_generated(const struct NvRemoteKV *remote);
uint64_t nv_remote_deadline(const NvSequence *sequence);
void nv_remote_wait(struct NvRemoteKV *remote, uint64_t deadline);
int nv_remote_upload_busy(const struct NvRemoteKV *remote);
void nv_remote_publish(NvEngine *engine, NvSequence *sequence);
void nv_remote_evict(struct NvRemoteKV *remote, int block);
void nv_remote_close(NvEngine *engine);

#endif
