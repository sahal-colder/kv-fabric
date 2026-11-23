/*
 * 通用 KVStore 回归层，覆盖 allocator。
 * 夹具拥有本地数据或连接。业务锁由被测代码管理。
 */
#include "kvs_allocator.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define THREAD_COUNT 4
#define SLOT_COUNT 64
#define ITERATION_COUNT 10000
#define MAX_ALLOCATION_SIZE 8192
#define CROSS_THREAD_COUNT 2048

typedef struct allocation_slot_s {
	unsigned char *data;
	size_t size;
	unsigned char marker;
} allocation_slot_t;

typedef struct stress_context_s {
	uint32_t random_state;
	int failed;
} stress_context_t;

typedef struct cross_thread_context_s {
	void **allocations;
	size_t *sizes;
	int failed;
} cross_thread_context_t;

static void *cross_thread_allocations[CROSS_THREAD_COUNT];
static size_t cross_thread_sizes[CROSS_THREAD_COUNT];

static uint32_t next_random(uint32_t *state) {
	uint32_t value = *state;

	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	*state = value;
	return value;
}

static int memory_has_marker(
	const unsigned char *data,
	size_t size,
	unsigned char marker
) {
	for (size_t i = 0; i < size; i++) {
		if (data[i] != marker) return 0;
	}
	return 1;
}

static void release_slots(allocation_slot_t slots[SLOT_COUNT]) {
	for (size_t i = 0; i < SLOT_COUNT; i++) {
		kvs_free(slots[i].data);
		slots[i].data = NULL;
	}
}

static void *stress_worker(void *argument) {
	stress_context_t *context = argument;
	allocation_slot_t slots[SLOT_COUNT] = {{0}};

	for (size_t iteration = 0; iteration < ITERATION_COUNT; iteration++) {
		uint32_t random_value = next_random(&context->random_state);
		allocation_slot_t *slot = &slots[random_value % SLOT_COUNT];

		if (slot->data == NULL) {
			slot->size = next_random(&context->random_state) %
				MAX_ALLOCATION_SIZE + 1U;
			slot->marker = (unsigned char)
				(next_random(&context->random_state) | 1U);
			slot->data = kvs_malloc(slot->size);
			if (slot->data == NULL) {
				context->failed = 1;
				break;
			}
			memset(slot->data, slot->marker, slot->size);
			continue;
		}

		if (!memory_has_marker(slot->data, slot->size, slot->marker)) {
			context->failed = 1;
			break;
		}

		if ((random_value & 3U) == 0) {
			kvs_free(slot->data);
			memset(slot, 0, sizeof(*slot));
		} else {
			size_t old_size = slot->size;
			size_t new_size = next_random(&context->random_state) %
				MAX_ALLOCATION_SIZE + 1U;
			unsigned char *replacement = kvs_realloc(slot->data, new_size);

			if (replacement == NULL) {
				context->failed = 1;
				break;
			}
			slot->data = replacement;
			slot->size = new_size;
			if (!memory_has_marker(
				replacement,
				old_size < new_size ? old_size : new_size,
				slot->marker
			)) {
				context->failed = 1;
				break;
			}
			slot->marker++;
			if (slot->marker == 0) slot->marker = 1;
			memset(slot->data, slot->marker, slot->size);
		}
	}

	for (size_t i = 0; i < SLOT_COUNT; i++) {
		if (slots[i].data != NULL && !memory_has_marker(
			slots[i].data,
			slots[i].size,
			slots[i].marker
		)) {
			context->failed = 1;
		}
	}
	release_slots(slots);
	return NULL;
}

static void *cross_thread_free_worker(void *argument) {
	cross_thread_context_t *context = argument;

	for (size_t i = 0; i < CROSS_THREAD_COUNT; i++) {
		unsigned char marker = (unsigned char)((i % 251U) + 1U);

		if (!memory_has_marker(
			context->allocations[i],
			context->sizes[i],
			marker
		)) {
			context->failed = 1;
		}
		kvs_free(context->allocations[i]);
		context->allocations[i] = NULL;
	}
	return NULL;
}

static int test_basic_operations(void) {
	unsigned char *data;
	unsigned char *replacement;
	void *first;
	void *second;

	if (kvs_malloc(0) != NULL ||
		kvs_calloc(0, 1) != NULL ||
		kvs_calloc(1, 0) != NULL ||
		kvs_calloc(SIZE_MAX, 2) != NULL ||
		kvs_realloc(NULL, 0) != NULL) {
		return -1;
	}

	data = kvs_calloc(256, 1);
	if (data == NULL) return -1;
	for (size_t i = 0; i < 256; i++) {
		if (data[i] != 0) {
			kvs_free(data);
			return -1;
		}
	}
	memset(data, 0x5a, 256);
	replacement = kvs_realloc(data, 8192);
	if (replacement == NULL || !memory_has_marker(replacement, 256, 0x5a)) {
		kvs_free(replacement == NULL ? data : replacement);
		return -1;
	}
	data = replacement;
	replacement = kvs_realloc(data, 32);
	if (replacement == NULL || !memory_has_marker(replacement, 32, 0x5a)) {
		kvs_free(replacement == NULL ? data : replacement);
		return -1;
	}
	if (kvs_realloc(replacement, 0) != NULL) return -1;

	for (size_t size = 1; size <= 4096; size++) {
		void *allocation = kvs_malloc(size);

		if (allocation == NULL ||
			(uintptr_t)allocation % _Alignof(max_align_t) != 0) {
			kvs_free(allocation);
			return -1;
		}
		kvs_free(allocation);
	}

	first = kvs_malloc(24);
	if (first == NULL) return -1;
	kvs_free(first);
	second = kvs_malloc(24);
	if (second == NULL) return -1;
#if KVS_ALLOCATOR_BACKEND == KVS_ALLOCATOR_MMPOOL
	if (second != first) {
		kvs_free(second);
		return -1;
	}
#endif
	kvs_free(second);
	return 0;
}

static int test_cross_thread_free(void) {
	pthread_t thread;
	cross_thread_context_t context = {
		cross_thread_allocations,
		cross_thread_sizes,
		0
	};

	for (size_t i = 0; i < CROSS_THREAD_COUNT; i++) {
		unsigned char marker = (unsigned char)((i % 251U) + 1U);

		cross_thread_sizes[i] = i % MAX_ALLOCATION_SIZE + 1U;
		cross_thread_allocations[i] = kvs_malloc(cross_thread_sizes[i]);
		if (cross_thread_allocations[i] == NULL) {
			for (size_t j = 0; j < i; j++) {
				kvs_free(cross_thread_allocations[j]);
				cross_thread_allocations[j] = NULL;
			}
			return -1;
		}
		memset(cross_thread_allocations[i], marker, cross_thread_sizes[i]);
	}

	if (pthread_create(&thread, NULL, cross_thread_free_worker, &context) != 0) {
		for (size_t i = 0; i < CROSS_THREAD_COUNT; i++) {
			kvs_free(cross_thread_allocations[i]);
			cross_thread_allocations[i] = NULL;
		}
		return -1;
	}
	if (pthread_join(thread, NULL) != 0) return -1;
	return context.failed ? -1 : 0;
}

static int test_concurrent_stress(void) {
	pthread_t threads[THREAD_COUNT];
	stress_context_t contexts[THREAD_COUNT] = {{0}};
	size_t created = 0;
	int failed = 0;

	for (size_t i = 0; i < THREAD_COUNT; i++) {
		contexts[i].random_state = 0x9e3779b9U ^ (uint32_t)(i + 1U);
		if (pthread_create(&threads[i], NULL, stress_worker, &contexts[i]) != 0) {
			failed = 1;
			break;
		}
		created++;
	}
	for (size_t i = 0; i < created; i++) {
		if (pthread_join(threads[i], NULL) != 0 || contexts[i].failed) {
			failed = 1;
		}
	}
	return failed ? -1 : 0;
}

int main(void) {
	int failed = 0;

	if (kvs_allocator_init() != 0 || kvs_allocator_init() != 0) {
		fprintf(stderr, "allocator initialization failed\n");
		return 1;
	}
	if (test_basic_operations() != 0) {
		fprintf(stderr, "allocator basic operations failed\n");
		failed = 1;
	}
	if (test_cross_thread_free() != 0) {
		fprintf(stderr, "allocator cross-thread free failed\n");
		failed = 1;
	}
	if (test_concurrent_stress() != 0) {
		fprintf(stderr, "allocator concurrent stress failed\n");
		failed = 1;
	}
	kvs_allocator_destroy();
	kvs_allocator_destroy();

	if (failed) return 1;
	printf("allocator: PASS\n");
	return 0;
}
