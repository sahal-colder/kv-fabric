/* 物理后端内部接口。调用方持有保留槽。锁顺序为 Storage -> Backend。 */
#ifndef MINIINFER_STORAGE_INTERNAL_H
#define MINIINFER_STORAGE_INTERNAL_H
#include "miniinfer/storage_backend.h"
#include "miniinfer/device_transfer.h"

/* 独占绑定后端，并返回预算允许使用的槽数。 */
mi_status mi_backend_attach(mi_storage_backend *backend, size_t limit, size_t *slots);
/* 解除绑定并恢复完整槽容量。 */
void mi_backend_detach(mi_storage_backend *backend);
/* 返回当前可分配的槽数。 */
size_t mi_backend_available(mi_storage_backend *backend);
/* 用有效的预留句柄直接计算 Slab 槽地址。 */
unsigned char *mi_backend_reserved_ptr(const mi_storage_backend *backend, mi_storage_handle handle);
/* 返回后端关联的设备编号。 */
int mi_backend_device(const mi_storage_backend *backend);
int mi_backend_is_direct(const mi_storage_backend *backend);
int mi_backend_gpu_pending(const mi_storage_backend *backend);
/* Caller pins the slot. DMA uses backend-owned host memory until confirmed. */
mi_status mi_backend_gpu_copy(mi_storage_backend *backend, mi_storage_handle slot,
                               size_t offset, void *host, size_t bytes, int write,
                               mi_stream stream, mi_event event);
/* Migration owns both addresses and retains both backends on pending DMA. */
mi_status mi_backend_gpu_copy_owned(mi_storage_backend *backend, mi_storage_handle slot,
                                     void *host, size_t bytes, int write);
mi_status mi_backend_direct_move(mi_storage_backend *ssd, mi_storage_handle disk,
                                  const mi_block_meta *meta, void *data, int device, int write);

#endif
