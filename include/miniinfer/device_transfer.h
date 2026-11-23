/*
 * 设备传输层的 C11 接口。调用方拥有内存、流和事件，并在释放前等待 DMA。
 * CPU 使用 device=-1。启用 CUDA 后使用非负设备编号，无虚表分发。
 * 锁顺序：运行时调用在 Storage/Trie/Directory 锁外执行。
 */
#ifndef MINIINFER_DEVICE_TRANSFER_H
#define MINIINFER_DEVICE_TRANSFER_H

#include "common.h"

typedef enum { MI_DEVICE_CPU, MI_DEVICE_CUDA } mi_device_kind;
typedef void *mi_event;

mi_device_kind mi_device_backend(void);
/* 每个设备资源所有者取得一次引用，所有线程和 DMA 退出后归还。 */
mi_status mi_device_init(int device);
mi_status mi_device_cleanup(int device);
mi_status mi_device_select(int device);
mi_status mi_device_synchronize(int device);
mi_status mi_device_alloc(int device, size_t bytes, void **out);
mi_status mi_device_free(int device, void *ptr);
mi_status mi_device_host_alloc(int device, size_t bytes, void **out);
mi_status mi_device_host_free(int device, void *ptr);
mi_status mi_device_stream_create(int device, mi_stream *out);
mi_status mi_device_stream_destroy(int device, mi_stream stream);
mi_status mi_device_stream_wait(int device, mi_stream stream);
mi_status mi_device_event_create(int device, mi_event *out);
mi_status mi_device_event_destroy(int device, mi_event event);
mi_status mi_device_event_record(int device, mi_event event, mi_stream stream);
mi_status mi_device_event_wait(int device, mi_event event);
mi_status mi_device_event_ready(int device, mi_event event);
/* 异步传输要求主机端使用对应运行时分配的缓冲区。NULL 流可用于同步拷贝。 */
mi_status mi_device_copy_to_host(int device, void *host, const void *src, size_t bytes, mi_stream stream);
mi_status mi_device_copy_from_host(int device, void *dst, const void *host, size_t bytes, mi_stream stream);

#endif
