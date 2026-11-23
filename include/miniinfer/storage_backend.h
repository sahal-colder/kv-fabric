/*
 * Storage 的物理槽接口。后端拥有槽表和文件，Slab 的底层池由调用方保管。
 * 锁顺序：Storage -> 槽元数据锁。实际磁盘与设备 I/O 在两者锁外。
 */
#ifndef MINIINFER_STORAGE_BACKEND_H
#define MINIINFER_STORAGE_BACKEND_H
#include "allocator.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct mi_storage_backend mi_storage_backend;

typedef struct mi_memory_view {
    /* 槽的内存地址。SSD 槽返回 NULL。 */
    void *data;
    /* 槽可访问的字节数，SSD 槽为 0。 */
    size_t capacity;
    /* 该物理槽所属的存储层。 */
    mi_location location;
    /* 后端关联的设备编号。 */
    int device_id;
} mi_memory_view;

typedef enum mi_ssd_verify {
    /* 读取时检查记录身份，跳过数据 CRC 比对。 */
    MI_SSD_VERIFY_NONE = 0,
    /* 读取时比对数据 CRC32C。 */
    MI_SSD_VERIFY_ON_READ = 1,
    /* 写入后读回验证，后续读取也比对 CRC32C。 */
    MI_SSD_VERIFY_READ_BACK = 2
} mi_ssd_verify;

/* 从内存池借用连续 Slab，按 slot_bytes 划分固定槽。内存池须晚于后端销毁。 */
mi_status mi_memory_backend_create(mi_location location, size_t slot_bytes,
                                   mi_mempool *pool, size_t max_slots,
                                   mi_storage_backend **out);

/* 创建 POSIX 文件后端。path 为 NULL 时使用临时文件。 */
mi_status mi_ssd_backend_create(const char *path, size_t slot_bytes,
                                size_t max_bytes, mi_ssd_verify verify,
                                mi_storage_backend **out);

/* 文件后端的定位 I/O 实现。 */
typedef enum mi_ssd_file_mode { MI_SSD_FILE_POSIX, MI_SSD_FILE_URING } mi_ssd_file_mode;

/* 按 mode 创建文件后端。传入已有路径时会重置文件内容。 */
mi_status mi_ssd_file_backend_create(const char *path, size_t slot_bytes, size_t max_bytes,
                                     mi_ssd_verify verify, mi_ssd_file_mode mode,
                                     unsigned queue_depth, mi_storage_backend **out);

/* 在 dir 建立私有临时后端，仅通过打开的描述符访问。 */
mi_status mi_ssd_temp_backend_create(const char *dir, size_t slot_bytes, size_t max_bytes,
                                     mi_ssd_verify verify, mi_ssd_file_mode mode,
                                     unsigned queue_depth, mi_storage_backend **out);

/* 对齐整块记录。device=-1 用于 CPU，其余值须与 Storage 加速器序号一致。 */
mi_status mi_ssd_direct_backend_create(const char *path, size_t slot_bytes, size_t max_bytes,
                                       mi_ssd_verify verify, unsigned depth, int device,
                                       mi_storage_backend **out);
const char *mi_ssd_file_io_name(const mi_storage_backend *backend);

typedef struct mi_spdk_config {
    /* SPDK 子系统配置文件路径。 */
    const char *json_config;
    /* 要打开的 SPDK 块设备名。 */
    const char *bdev_name;
    /* reactor 的 CPU 核掩码。 */
    const char *reactor_mask;
    /* 设备区域的起始字节偏移，须按 4 KiB 对齐。 */
    uint64_t offset_bytes;
    /* 可用设备区域字节数，须按 4 KiB 对齐。 */
    size_t capacity_bytes;
    /* 禁用大页内存。 */
    int no_huge;
    /* 禁用 PCI 枚举。 */
    int no_pci;
} mi_spdk_config;

/* 在指定设备区间内创建 SSD 后端。 */
mi_status mi_ssd_spdk_backend_create(mi_spdk_config config, size_t slot_bytes,
                                     mi_ssd_verify verify, mi_storage_backend **out);

/* 返回后端所属层。 */
mi_location mi_backend_location(const mi_storage_backend *backend);
/* 返回单个槽可容纳的数据字节数。 */
size_t mi_backend_slot_bytes(const mi_storage_backend *backend);
/* 返回含记录头与对齐开销的槽步长，超出槽大小时返回 0。 */
size_t mi_backend_allocation_bytes(const mi_storage_backend *backend, size_t payload_bytes);

/* 预留物理槽，返回带代数的句柄。调用方须协调同一槽的写入、封存和释放。 */
mi_status mi_backend_reserve(mi_storage_backend *backend, size_t payload_bytes,
                             mi_storage_handle *out);
/* 回收物理槽，使当前句柄失效。 */
mi_status mi_backend_release(mi_storage_backend *backend, mi_storage_handle handle);
/* 查询槽的地址和容量。视图只在槽有效时可用。 */
mi_status mi_backend_memory_view(mi_storage_backend *backend, mi_storage_handle handle,
                                 mi_memory_view *out);
/* 按槽内偏移读取。SSD 记录须已封存。 */
mi_status mi_backend_read(mi_storage_backend *backend, mi_storage_handle handle,
                          size_t offset, void *host, size_t bytes);
/* 按槽内偏移写入。已封存的 SSD 记录禁止覆写。 */
mi_status mi_backend_write(mi_storage_backend *backend, mi_storage_handle handle,
                           size_t offset, const void *host, size_t bytes);

/* 为已写入的 SSD 数据补上记录头和校验值。 */
mi_status mi_backend_publish(mi_storage_backend *backend, mi_storage_handle handle,
                             const mi_block_meta *meta);

/* 将数据和记录头一起写入并封存。 */
mi_status mi_backend_write_record(mi_storage_backend *backend, mi_storage_handle handle,
                                  const mi_block_meta *meta, const void *host);
/* 读入数据并核对 SSD 记录身份及配置要求的校验值。 */
mi_status mi_backend_read_validated(mi_storage_backend *backend, mi_storage_handle handle,
                                    const mi_block_meta *expected, void *host);
/* 关闭 I/O 资源或向内存池归还 Slab。 */
void mi_backend_destroy(mi_storage_backend *backend);

#ifdef __cplusplus
}
#endif
#endif
