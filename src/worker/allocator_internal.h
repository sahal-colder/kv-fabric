/* Physical backends retain their borrowed pool until their slab is released. */
#ifndef MINIINFER_ALLOCATOR_INTERNAL_H
#define MINIINFER_ALLOCATOR_INTERNAL_H
#include "miniinfer/allocator.h"
void mi_mempool_hold(mi_mempool *pool);
void mi_mempool_drop(mi_mempool *pool);
#endif
