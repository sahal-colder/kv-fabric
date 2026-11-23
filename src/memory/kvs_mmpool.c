/*
 * 内存层。按大小类管理块缓存和大块分配。
 * 池拥有底层页和空闲链，调用方独占借用块。池锁不回调上层。
 */
#include "kvs_mmpool.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MP_MIN_CLASS_SIZE 16U
#define MP_SMALL_MAX 4096U
#define MP_CLASS_COUNT 9U
#define MP_MAX_CHUNK_SIZE (64U * 1024U)

#define MP_BLOCK_LIVE 0x4d504c56U
#define MP_BLOCK_FREE 0x4d504652U

#define MP_BLOCK_SMALL 1U
#define MP_BLOCK_LARGE 2U

#define MP_ALIGNMENT ((size_t)_Alignof(max_align_t))

typedef union mp_block_u mp_block_t;

union mp_block_u {
	struct {
		mp_block_t *next;
		mp_block_t *previous;
		void *allocation;
		size_t capacity;
		size_t requested;
		uint32_t magic;
		uint16_t class_index;
		uint16_t kind;
	} info;
	max_align_t alignment;
};

struct mp_chunk_s {
	struct mp_chunk_s *next;
};

struct mp_pool_s {
	size_t chunk_size;
	mp_block_t *free_lists[MP_CLASS_COUNT];
	mp_block_t *large;
	struct mp_chunk_s *chunks;
	pthread_mutex_t mutex;
};

static int mp_size_add(size_t left, size_t right, size_t *result) {
	if (left > SIZE_MAX - right) return -1;
	*result = left + right;
	return 0;
}

static uintptr_t mp_align_address(uintptr_t address, size_t alignment) {
	return (address + alignment - 1U) & ~(uintptr_t)(alignment - 1U);
}

static int mp_alignment_valid(size_t alignment) {
	return alignment != 0 && (alignment & (alignment - 1U)) == 0;
}

static size_t mp_class_capacity(size_t size, size_t *class_index) {
	size_t capacity = MP_MIN_CLASS_SIZE;
	size_t index = 0;

	while (capacity < size) {
		capacity <<= 1U;
		index++;
	}
	*class_index = index;
	return capacity;
}

static void mp_link_large(struct mp_pool_s *pool, mp_block_t *block) {
	block->info.previous = NULL;
	block->info.next = pool->large;
	if (pool->large != NULL) pool->large->info.previous = block;
	pool->large = block;
}

static void mp_unlink_large(struct mp_pool_s *pool, mp_block_t *block) {
	if (block->info.previous != NULL)
		block->info.previous->info.next = block->info.next;
	else
		pool->large = block->info.next;
	if (block->info.next != NULL)
		block->info.next->info.previous = block->info.previous;
}

static int mp_refill_class(
	struct mp_pool_s *pool,
	size_t class_index,
	size_t capacity
) {
	size_t stride;
	size_t target;
	size_t block_count;
	size_t data_size;
	size_t allocation_size;
	struct mp_chunk_s *chunk;
	uintptr_t start;

	if (mp_size_add(sizeof(mp_block_t), capacity, &stride) != 0) return -1;
	target = pool->chunk_size / MP_CLASS_COUNT;
	if (target < stride) target = stride;
	if (target > MP_MAX_CHUNK_SIZE) target = MP_MAX_CHUNK_SIZE;
	block_count = target / stride;
	if (block_count == 0) block_count = 1;
	if (block_count > SIZE_MAX / stride) return -1;
	data_size = block_count * stride;
	if (mp_size_add(sizeof(*chunk), MP_ALIGNMENT - 1U, &allocation_size) != 0 ||
		mp_size_add(allocation_size, data_size, &allocation_size) != 0) {
		return -1;
	}

	chunk = malloc(allocation_size);
	if (chunk == NULL) return -1;
	chunk->next = pool->chunks;
	pool->chunks = chunk;
	start = mp_align_address(
		(uintptr_t)(unsigned char *)chunk + sizeof(*chunk),
		MP_ALIGNMENT
	);

	for (size_t i = 0; i < block_count; i++) {
		mp_block_t *block = (mp_block_t *)(start + i * stride);

		block->info.next = pool->free_lists[class_index];
		block->info.previous = NULL;
		block->info.allocation = NULL;
		block->info.capacity = capacity;
		block->info.requested = 0;
		block->info.magic = MP_BLOCK_FREE;
		block->info.class_index = (uint16_t)class_index;
		block->info.kind = MP_BLOCK_SMALL;
		pool->free_lists[class_index] = block;
	}
	return 0;
}

static void *mp_alloc_large_unlocked(
	struct mp_pool_s *pool,
	size_t size,
	size_t alignment
) {
	size_t allocation_size;
	void *allocation;
	uintptr_t payload_address;
	mp_block_t *block;

	if (alignment < MP_ALIGNMENT) alignment = MP_ALIGNMENT;
	if (mp_size_add(sizeof(mp_block_t), alignment - 1U, &allocation_size) != 0 ||
		mp_size_add(allocation_size, size, &allocation_size) != 0) {
		return NULL;
	}

	allocation = malloc(allocation_size);
	if (allocation == NULL) return NULL;
	payload_address = mp_align_address(
		(uintptr_t)(unsigned char *)allocation + sizeof(mp_block_t),
		alignment
	);
	block = (mp_block_t *)(payload_address - sizeof(mp_block_t));
	block->info.allocation = allocation;
	block->info.capacity = size;
	block->info.requested = size;
	block->info.magic = MP_BLOCK_LIVE;
	block->info.class_index = UINT16_MAX;
	block->info.kind = MP_BLOCK_LARGE;
	mp_link_large(pool, block);
	return (void *)payload_address;
}

static void *mp_alloc_unlocked(struct mp_pool_s *pool, size_t size) {
	size_t class_index;
	size_t capacity;
	mp_block_t *block;

	if (size == 0) return NULL;
	if (size > MP_SMALL_MAX)
		return mp_alloc_large_unlocked(pool, size, MP_ALIGNMENT);

	capacity = mp_class_capacity(size, &class_index);
	if (pool->free_lists[class_index] == NULL &&
		mp_refill_class(pool, class_index, capacity) != 0) {
		return NULL;
	}

	block = pool->free_lists[class_index];
	pool->free_lists[class_index] = block->info.next;
	block->info.next = NULL;
	block->info.requested = size;
	block->info.magic = MP_BLOCK_LIVE;
	return (void *)(block + 1);
}

static void mp_free_unlocked(struct mp_pool_s *pool, void *ptr) {
	mp_block_t *block;

	if (ptr == NULL) return;
	block = (mp_block_t *)ptr - 1;
	if (block->info.magic != MP_BLOCK_LIVE) return;

	if (block->info.kind == MP_BLOCK_SMALL &&
		block->info.class_index < MP_CLASS_COUNT) {
		size_t class_index = block->info.class_index;

		block->info.magic = MP_BLOCK_FREE;
		block->info.requested = 0;
		block->info.next = pool->free_lists[class_index];
		pool->free_lists[class_index] = block;
		return;
	}
	if (block->info.kind == MP_BLOCK_LARGE) {
		void *allocation = block->info.allocation;

		mp_unlink_large(pool, block);
		block->info.magic = MP_BLOCK_FREE;
		free(allocation);
	}
}

static void mp_reset_unlocked(struct mp_pool_s *pool) {
	mp_block_t *large = pool->large;
	struct mp_chunk_s *chunk = pool->chunks;

	while (large != NULL) {
		mp_block_t *next = large->info.next;
		free(large->info.allocation);
		large = next;
	}
	while (chunk != NULL) {
		struct mp_chunk_s *next = chunk->next;
		free(chunk);
		chunk = next;
	}
	memset(pool->free_lists, 0, sizeof(pool->free_lists));
	pool->large = NULL;
	pool->chunks = NULL;
}

struct mp_pool_s *mp_create_pool(size_t size) {
	struct mp_pool_s *pool;

	if (size == 0) return NULL;
	pool = calloc(1, sizeof(*pool));
	if (pool == NULL) return NULL;
	pool->chunk_size = size;
	if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
		free(pool);
		return NULL;
	}
	return pool;
}

void mp_destroy_pool(struct mp_pool_s *pool) {
	if (pool == NULL) return;
	pthread_mutex_lock(&pool->mutex);
	mp_reset_unlocked(pool);
	pthread_mutex_unlock(&pool->mutex);
	pthread_mutex_destroy(&pool->mutex);
	free(pool);
}

void mp_destory_pool(struct mp_pool_s *pool) {
	mp_destroy_pool(pool);
}

void mp_reset_pool(struct mp_pool_s *pool) {
	if (pool == NULL) return;
	pthread_mutex_lock(&pool->mutex);
	mp_reset_unlocked(pool);
	pthread_mutex_unlock(&pool->mutex);
}

void *mp_alloc(struct mp_pool_s *pool, size_t size) {
	void *result;

	if (pool == NULL || size == 0) return NULL;
	pthread_mutex_lock(&pool->mutex);
	result = mp_alloc_unlocked(pool, size);
	pthread_mutex_unlock(&pool->mutex);
	return result;
}

void *mp_nalloc(struct mp_pool_s *pool, size_t size) {
	return mp_alloc(pool, size);
}

void *mp_calloc(struct mp_pool_s *pool, size_t size) {
	void *result;

	if (pool == NULL || size == 0) return NULL;
	pthread_mutex_lock(&pool->mutex);
	result = mp_alloc_unlocked(pool, size);
	pthread_mutex_unlock(&pool->mutex);
	if (result != NULL) memset(result, 0, size);
	return result;
}

void *mp_realloc(struct mp_pool_s *pool, void *ptr, size_t size) {
	mp_block_t *block;
	void *replacement;
	size_t copy_size;

	if (pool == NULL) return NULL;
	pthread_mutex_lock(&pool->mutex);
	if (ptr == NULL) {
		replacement = mp_alloc_unlocked(pool, size);
		pthread_mutex_unlock(&pool->mutex);
		return replacement;
	}
	if (size == 0) {
		mp_free_unlocked(pool, ptr);
		pthread_mutex_unlock(&pool->mutex);
		return NULL;
	}

	block = (mp_block_t *)ptr - 1;
	if (block->info.magic != MP_BLOCK_LIVE) {
		pthread_mutex_unlock(&pool->mutex);
		return NULL;
	}
	if (size <= block->info.capacity) {
		block->info.requested = size;
		pthread_mutex_unlock(&pool->mutex);
		return ptr;
	}

	replacement = mp_alloc_unlocked(pool, size);
	if (replacement != NULL) {
		copy_size = block->info.requested;
		memcpy(replacement, ptr, copy_size);
		mp_free_unlocked(pool, ptr);
	}
	pthread_mutex_unlock(&pool->mutex);
	return replacement;
}

void *mp_memalign(struct mp_pool_s *pool, size_t size, size_t alignment) {
	void *result;

	if (pool == NULL || size == 0 || !mp_alignment_valid(alignment)) return NULL;
	if (alignment <= MP_ALIGNMENT) return mp_alloc(pool, size);
	pthread_mutex_lock(&pool->mutex);
	result = mp_alloc_large_unlocked(pool, size, alignment);
	pthread_mutex_unlock(&pool->mutex);
	return result;
}

void mp_free(struct mp_pool_s *pool, void *ptr) {
	if (pool == NULL || ptr == NULL) return;
	pthread_mutex_lock(&pool->mutex);
	mp_free_unlocked(pool, ptr);
	pthread_mutex_unlock(&pool->mutex);
}
