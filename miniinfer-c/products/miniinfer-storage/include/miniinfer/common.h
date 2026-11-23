/*
 * 存储公共类型层。定义状态码、逻辑 Block ID、位置和版本元数据。
 * 值类型不持有分配或锁。
 */
#ifndef MINIINFER_COMMON_H
#define MINIINFER_COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#error "MiniInfer C storage core currently targets POSIX/Linux"
#endif

/* 存储 API 状态码。MI_OK 为 0。 */
typedef enum mi_status {
    MI_OK = 0,
    /* 参数或访问范围无效。 */
    MI_EINVAL,
    /* 内存不足或容量耗尽。 */
    MI_ENOMEM,
    /* 块或句柄不存在。 */
    MI_ENOENT,
    /* 资源正在使用。 */
    MI_EBUSY,
    /* 当前状态不允许该操作。 */
    MI_ESTATE,
    /* 底层 I/O 或设备操作失败。 */
    MI_EIO,
    /* SSD 记录身份或校验值不匹配。 */
    MI_ECRC,
    /* 数值超出可表示范围。 */
    MI_EOVERFLOW,
    /* 当前构建或后端不支持该操作。 */
    MI_EUNSUPPORTED,
    /* 操作已提交，旧资源仍待清理。 */
    MI_ECLEANUP,
    /* 内部初始化或同步失败。 */
    MI_EINTERNAL,
    /* 操作未提交，回滚时仍有资源待清理。 */
    MI_EROLLBACK_CLEANUP
} mi_status;

/* 返回用于日志的状态说明。 */
const char *mi_status_string(mi_status status);

/* 块的驻留层。REMOTE 为预留值。 */
typedef enum mi_location {
    MI_LOCATION_CPU = 0,
    MI_LOCATION_GPU = 1,
    MI_LOCATION_SSD = 2,
    MI_LOCATION_REMOTE = 3
} mi_location;

/* 块的生命周期状态。是否可读还取决于初始化和租约。 */
typedef enum mi_block_state {
    MI_BLOCK_RESIDENT = 0,
    MI_BLOCK_MOVING,
    MI_BLOCK_EVICTED,
    MI_BLOCK_ERROR
} mi_block_state;

/* 读租约可共享，写租约独占。 */
typedef enum mi_access_mode {
    MI_ACCESS_READ = 0,
    MI_ACCESS_WRITE = 1
} mi_access_mode;

/* 逻辑块 ID，随块迁移保持稳定。 */
typedef uint64_t mi_block_id;
/* 后端物理槽句柄，只在所属后端内有效。 */
typedef uint64_t mi_storage_handle;
/* ID 和句柄统一以 0 表示无效。 */
#define MI_INVALID_BLOCK_ID ((mi_block_id)0)
#define MI_INVALID_HANDLE ((mi_storage_handle)0)

/* 加速器流句柄的通用表示。 */
typedef void *mi_stream;

typedef struct mi_block_meta {
    /* 跨存储层使用的逻辑 ID。 */
    mi_block_id id;
    /* 数据当前所在的存储层。 */
    mi_location location;
    /* 有效数据字节数。 */
    size_t size;
    /* 成功提交写租约时递增的版本号。 */
    uint64_t version;
    /* 活跃租约数量。 */
    uint32_t lease_count;
    /* 当前块状态。 */
    mi_block_state state;
} mi_block_meta;

#ifdef __cplusplus
}
#endif
#endif
