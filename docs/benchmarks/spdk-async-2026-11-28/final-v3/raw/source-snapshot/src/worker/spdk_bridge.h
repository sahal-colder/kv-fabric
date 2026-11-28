/* Single owning SPDK thread; callers borrow ranges until wait returns. */
#ifndef MINIINFER_SPDK_BRIDGE_H
#define MINIINFER_SPDK_BRIDGE_H
#include "miniinfer/storage_backend.h"
#include "io_transfer.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct mi_spdk_device mi_spdk_device;
typedef struct mi_spdk_request mi_spdk_request;
typedef struct mi_spdk_stats {
    uint64_t submitted, completed, requests, finished, queue_waits, pool_waits, copy_ns;
    unsigned inflight, max_inflight, active_requests, max_active_requests;
} mi_spdk_stats;
mi_status mi_spdk_open(mi_spdk_config config, mi_spdk_device **out);
mi_status mi_spdk_open_ex(mi_spdk_config config, mi_spdk_options options, mi_spdk_device **out);
/* Nonblocking pool acquisition: EBUSY means no request/DMA slot is free.
 * Overlapping writes (including sectors touched by legacy RMW) need caller exclusion. */
mi_status mi_spdk_submit(mi_spdk_device *device, int write, mi_io_range range, mi_spdk_request **out);
/* wait consumes the request; exactly one waiter. cancel does not consume it.
 * Cancellation returns EBUSY after current DMA completes; prior writes may persist. */
mi_status mi_spdk_wait(mi_spdk_request *request);
mi_status mi_spdk_wait_metrics(mi_spdk_request *request, mi_io_metrics *metrics);
void mi_spdk_cancel(mi_spdk_request *request);
mi_status mi_spdk_transfer(mi_spdk_device *device, int write, mi_io_range range);
mi_status mi_spdk_transfer_batch(mi_spdk_device *device, int write, const mi_io_range *ranges,
                                 size_t count, mi_status *results);
void mi_spdk_get_stats(mi_spdk_device *device, mi_spdk_stats *out);
/* Stop producers before close. Accepted requests must still be waited by their owners.
 * Shutdown drains callbacks; it never frees DMA on an elapsed timeout. */
void mi_spdk_close(mi_spdk_device *device);
#ifdef __cplusplus
}
#endif
#endif
