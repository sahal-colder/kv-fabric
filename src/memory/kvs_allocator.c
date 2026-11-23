/*
 * 内存层。按编译选项选择 libc、jemalloc 或 MMPOOL。
 * 分配返回的内存由调用方释放。池内部锁处于锁序末端。
 */
#include "kvs_allocator.h"

#include <stdint.h>
#include <stdlib.h>

#if KVS_ALLOCATOR_BACKEND == KVS_ALLOCATOR_MMPOOL
#include "kvs_mmpool.h"

#ifndef KVS_MMPOOL_SIZE
#define KVS_MMPOOL_SIZE (1024U * 1024U)
#endif

static struct mp_pool_s *kvs_pool;

int kvs_allocator_init(void) {
	if (kvs_pool != NULL) return 0;

	kvs_pool = mp_create_pool(KVS_MMPOOL_SIZE);
	return kvs_pool == NULL ? -1 : 0;
}

void kvs_allocator_destroy(void) {
	mp_destroy_pool(kvs_pool);
	kvs_pool = NULL;
}

void *kvs_malloc(size_t size) {
	return mp_alloc(kvs_pool, size);
}

void *kvs_calloc(size_t count, size_t size) {
	if (count == 0 || size == 0 || count > SIZE_MAX / size) return NULL;
	return mp_calloc(kvs_pool, count * size);
}

void *kvs_realloc(void *ptr, size_t size) {
	return mp_realloc(kvs_pool, ptr, size);
}

void kvs_free(void *ptr) {
	mp_free(kvs_pool, ptr);
}

#elif KVS_ALLOCATOR_BACKEND == KVS_ALLOCATOR_JEMALLOC

#include <jemalloc/jemalloc.h>

/* Match the allocator interface's max_align_t alignment contract. */
#define KVS_JEMALLOC_FLAGS MALLOCX_ALIGN(_Alignof(max_align_t))

int kvs_allocator_init(void) {
	return 0;
}

void kvs_allocator_destroy(void) {
}

void *kvs_malloc(size_t size) {
	if (size == 0) return NULL;
	return mallocx(size, KVS_JEMALLOC_FLAGS);
}

void *kvs_calloc(size_t count, size_t size) {
	if (count == 0 || size == 0 || count > SIZE_MAX / size) return NULL;
	return mallocx(count * size, KVS_JEMALLOC_FLAGS | MALLOCX_ZERO);
}

void *kvs_realloc(void *ptr, size_t size) {
	if (ptr == NULL) return size == 0 ? NULL : mallocx(size, KVS_JEMALLOC_FLAGS);
	if (size == 0) {
		dallocx(ptr, 0);
		return NULL;
	}
	return rallocx(ptr, size, KVS_JEMALLOC_FLAGS);
}

void kvs_free(void *ptr) {
	if (ptr != NULL) dallocx(ptr, 0);
}

#elif KVS_ALLOCATOR_BACKEND == KVS_ALLOCATOR_LIBC

int kvs_allocator_init(void) {
	return 0;
}

void kvs_allocator_destroy(void) {
}

void *kvs_malloc(size_t size) {
	if (size == 0) return NULL;
	return malloc(size);
}

void *kvs_calloc(size_t count, size_t size) {
	if (count == 0 || size == 0 || count > SIZE_MAX / size) return NULL;
	return calloc(count, size);
}

void *kvs_realloc(void *ptr, size_t size) {
	if (size == 0) {
		free(ptr);
		return NULL;
	}
	return realloc(ptr, size);
}

void kvs_free(void *ptr) {
	free(ptr);
}

#endif
