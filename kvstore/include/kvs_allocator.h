/*
 * 内存层。按编译选项选择 libc、jemalloc 或 MMPOOL。
 * 分配返回的内存由调用方释放。池内部锁处于锁序末端。
 */
#ifndef KVS_ALLOCATOR_H
#define KVS_ALLOCATOR_H

#include <stddef.h>

#define KVS_ALLOCATOR_LIBC      0  /* malloc/calloc/free */
#define KVS_ALLOCATOR_JEMALLOC  1  /* mallocx/dallocx */
#define KVS_ALLOCATOR_MMPOOL    2  /* custom memory pool */

/* KVS_ALLOCATOR_BACKEND selects the allocator implementation. */
#ifndef KVS_ALLOCATOR_BACKEND
/* Default to the thread-safe jemalloc backend. */
#define KVS_ALLOCATOR_BACKEND KVS_ALLOCATOR_JEMALLOC
#endif

#if KVS_ALLOCATOR_BACKEND != KVS_ALLOCATOR_LIBC && \
	KVS_ALLOCATOR_BACKEND != KVS_ALLOCATOR_JEMALLOC && \
	KVS_ALLOCATOR_BACKEND != KVS_ALLOCATOR_MMPOOL
#error "invalid KVS_ALLOCATOR_BACKEND"
#endif

int kvs_allocator_init(void);
void kvs_allocator_destroy(void);
void *kvs_malloc(size_t size);
void *kvs_calloc(size_t count, size_t size);
void *kvs_realloc(void *ptr, size_t size);
void kvs_free(void *ptr);

#endif
