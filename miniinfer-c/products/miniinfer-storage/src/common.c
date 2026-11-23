/*
 * 存储公共类型层。定义状态码、逻辑 Block ID、位置和版本元数据。
 * 值类型不持有分配或锁。
 */
#include "miniinfer/common.h"

/* 将存储状态码转换为日志文本。 */
const char *mi_status_string(mi_status s) {
    switch (s) {
    case MI_OK:
        return "success";
    case MI_EINVAL:
        return "invalid argument";
    case MI_ENOMEM:
        return "out of memory";
    case MI_ENOENT:
        return "not found";
    case MI_EBUSY:
        return "resource busy";
    case MI_ESTATE:
        return "invalid state";
    case MI_EIO:
        return "I/O error";
    case MI_ECRC:
        return "checksum mismatch";
    case MI_EOVERFLOW:
        return "integer overflow";
    case MI_EUNSUPPORTED:
        return "unsupported operation";
    case MI_ECLEANUP:
        return "operation committed; cleanup pending";
    case MI_EROLLBACK_CLEANUP:
        return "migration rolled back, target cleanup pending";
    default:
        return "internal error";
    }
}
