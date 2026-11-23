/*
 * 内存层。按大小类管理块缓存和大块分配。
 * 池拥有底层页和空闲链，调用方独占借用块。池锁不回调上层。
 */
#ifndef KVS_MMPOOL_H
#define KVS_MMPOOL_H

#include <stddef.h>

struct mp_pool_s;

struct mp_pool_s *mp_create_pool(size_t size);
void mp_destroy_pool(struct mp_pool_s *pool);
void mp_destory_pool(struct mp_pool_s *pool);
void mp_reset_pool(struct mp_pool_s *pool);
void *mp_alloc(struct mp_pool_s *pool, size_t size);
void *mp_nalloc(struct mp_pool_s *pool, size_t size);
void *mp_calloc(struct mp_pool_s *pool, size_t size);
void *mp_realloc(struct mp_pool_s *pool, void *ptr, size_t size);
void *mp_memalign(struct mp_pool_s *pool, size_t size, size_t alignment);
void mp_free(struct mp_pool_s *pool, void *p);

#endif
