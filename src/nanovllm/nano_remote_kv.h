#ifndef NANO_REMOTE_KV_H
#define NANO_REMOTE_KV_H

#include "nv.h"

/* All entry points are driven by the Nano-VLLM scheduling thread. The
 * publication thread consumes owned snapshots and never reads Nano pages. */
int nv_remote_begin(NvEngine *engine, NvSequence *sequence);
int nv_remote_poll(NvEngine *engine, NvSequence *sequence);
void nv_remote_cancel(NvEngine *engine, NvSequence *sequence);
void nv_remote_reap(struct NvRemoteKV *remote);
void nv_remote_publish(NvEngine *engine, NvSequence *sequence);
void nv_remote_evict(struct NvRemoteKV *remote, int block);
void nv_remote_close(NvEngine *engine);

#endif
