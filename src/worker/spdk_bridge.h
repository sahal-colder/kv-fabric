/*
 * 可选 SSD I/O 层。管理 SPDK bdev 渠道和专用轮询线程。
 * 桥接对象持有设备和 DMA 资源。数据请求在 Storage 锁外提交。
 */
#ifndef MINIINFER_SPDK_BRIDGE_H
#define MINIINFER_SPDK_BRIDGE_H

#include "miniinfer/storage_backend.h"
#include "io_transfer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mi_spdk_device mi_spdk_device;
/* 启动 reactor 并打开块设备。 */
mi_status mi_spdk_open(mi_spdk_config config, mi_spdk_device **out);
/* 在 reactor 上执行读写并等待结果。 */
mi_status mi_spdk_transfer(mi_spdk_device *device, int write, mi_io_range range);
/* 关闭设备并等待 reactor 退出。 */
void mi_spdk_close(mi_spdk_device *device);

#ifdef __cplusplus
}
#endif
#endif
