/* infer/base.h -- 第 0 层：不分配内存、不做 I/O、不创建线程，只放所有层共用的小工具。
 *
 * 全库约定：函数成功返回 >= 0，失败返回 -errno。不抛异常，也不靠全局 errno 传错。
 * 依赖方向：base <- mem <- io <- store <- abi。上层可以用下层，反过来不行。
 */
#ifndef INFER_BASE_H
#define INFER_BASE_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 声明导出符号。 */
#define IF_API __attribute__((visibility("default")))
/* 将条件标记为通常为真。 */
#define IF_LIKELY(x) __builtin_expect(!!(x), 1)
/* 将条件标记为通常为假。 */
#define IF_UNLIKELY(x) __builtin_expect(!!(x), 0)
/* 向上取整到二次幂对齐边界。 */
#define IF_ALIGN_UP(x, a) ((((x) + (a)-1)) & ~(__typeof__((x) + (a)))((a)-1))
/* 检查 x 是否为非零二次幂。 */
#define IF_IS_POW2(x) ((x) && !((x) & ((x)-1)))
/* 由成员地址恢复所属对象地址。 */
#define IF_CONTAINER_OF(p, T, m) ((T *)((char *)(p)-offsetof(T, m)))
/* 保存加法结果并报告溢出。 */
#define IF_ADD_OVF(a, b, out) __builtin_add_overflow((a), (b), (out))
/* 保存乘法结果并报告溢出。 */
#define IF_MUL_OVF(a, b, out) __builtin_mul_overflow((a), (b), (out))
/* 双向环形链表节点，嵌入调用方对象。 */
typedef struct if_node {
    struct if_node *prev, *next;
} if_node;

/* 初始化空链表的哨兵。 */
static inline void if_list_init(if_node *head) { head->prev = head->next = head; }
/* 检查链表是否只有哨兵。 */
static inline int if_list_empty(const if_node *head) { return head->next == head; }
/* 插入到链表头。 */
static inline void if_list_add(if_node *head, if_node *n) {
    n->next = head->next;
    n->prev = head;
    head->next->prev = n;
    head->next = n;
}
/* 插入到链表尾。 */
static inline void if_list_add_tail(if_node *head, if_node *n) {
    n->prev = head->prev;
    n->next = head;
    head->prev->next = n;
    head->prev = n;
}
/* 摘下节点并重置其链接。 */
static inline void if_list_del(if_node *n) {
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->prev = n->next = n;
}
/* 将位图中的第 i 位设为 1。 */
static inline void if_bit_set(uint64_t *w, unsigned i) { w[i >> 6] |= 1ull << (i & 63); }
/* 清除位图中的第 i 位。 */
static inline void if_bit_clear(uint64_t *w, unsigned i) { w[i >> 6] &= ~(1ull << (i & 63)); }
/* 返回位图中的第 i 位。 */
static inline int if_bit_test(const uint64_t *w, unsigned i) { return (int)((w[i >> 6] >> (i & 63)) & 1); }
/* 查找从 from 起的第一个置位，未找到时返回 -1。 */
static inline int if_bit_next(const uint64_t *w, unsigned nbits, unsigned from) {
    if (from >= nbits)
        return -1;
    unsigned i = from >> 6, words = (nbits + 63) >> 6;
    uint64_t x = w[i] & (~0ull << (from & 63));
    while (!x) {
        if (++i == words)
            return -1;
        x = w[i];
    }
    unsigned b = (i << 6) + (unsigned)__builtin_ctzll(x);
    return b < nbits ? (int)b : -1;
}

/* CRC32C (Castagnoli)。crc 传 0 开始；把上一次的返回值传回来可以分段续算。
 * if_crc32c 在 x86-64 上运行时选 SSE4.2 硬件指令（三路并行），否则用纯 C 查表。 */
IF_API uint32_t if_crc32c(uint32_t crc, const void *data, size_t n);
/* 始终使用软件查表计算 CRC32C。 */
IF_API uint32_t if_crc32c_sw(uint32_t crc, const void *data, size_t n);
/* 查询当前 CPU 是否支持 CRC32C 硬件路径。 */
IF_API int if_crc32c_hw(void);
#ifdef __cplusplus
}
#endif
#endif
