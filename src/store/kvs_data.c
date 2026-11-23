/*
 * 通用存储层。管理索引生命周期和按引擎分发，支持完整状态交换。
 * Store 拥有各索引。运行期调用方持 Store 读写锁。
 */
#include "kvs_store.h"

#include <stddef.h>

kvs_store_t kvs_store;

int kvs_store_create(kvs_store_t *store) {
	*store = (kvs_store_t){0};
	if (!kvs_array_create(&store->array) &&
	    !kvs_rbtree_create(&store->rbtree) &&
	    !kvs_hash_create(&store->hash) &&
	    !kvs_skiplist_create(&store->skiptable))
		return 0;
	kvs_store_destroy(store);
	return -1;
}

void kvs_store_destroy(kvs_store_t *store) {
	kvs_skiplist_destroy(&store->skiptable);
	kvs_hash_destroy(&store->hash);
	kvs_rbtree_destroy(&store->rbtree);
	kvs_array_destroy(&store->array);
}

void kvs_store_swap(kvs_store_t *staged) {
	kvs_store_t previous = kvs_store;
	kvs_store = *staged;
	*staged = previous;
}

const char *kvs_store_get(kvs_store_t *store, int engine, const char *key) {
	switch (engine) {
	case KVS_FORMAT_ENGINE_ARRAY:
		return kvs_array_get(&store->array, key);
	case KVS_FORMAT_ENGINE_RBTREE:
		return kvs_rbtree_get(&store->rbtree, key);
	case KVS_FORMAT_ENGINE_HASH:
		return kvs_hash_get(&store->hash, key);
	case KVS_FORMAT_ENGINE_SKIPTABLE: {
		skiptablenode_t *node = kvs_skiplist_get(&store->skiptable, key);
		return node ? node->value : NULL;
	}
	default:
		return NULL;
	}
}

static int tree_count(const rbtree *tree, const rbtree_node *node) {
	if (node == tree->nil)
		return 0;
	return 1 + tree_count(tree, node->left) + tree_count(tree, node->right);
}

int kvs_store_count(kvs_store_t *store, int engine) {
	int count = 0;
	switch (engine) {
	case KVS_FORMAT_ENGINE_ARRAY:
		for (int i = 0; i < store->array.total; i++)
			count += store->array.table[i].key != NULL;
		return count;
	case KVS_FORMAT_ENGINE_RBTREE:
		return tree_count(&store->rbtree, store->rbtree.root);
	case KVS_FORMAT_ENGINE_HASH:
		return store->hash.count;
	case KVS_FORMAT_ENGINE_SKIPTABLE:
		for (skiptablenode_t *node = store->skiptable.header->forward[0]; node;
		     node = node->forward[0])
			count++;
		return count;
	default:
		return 0;
	}
}

int kvs_store_mutate(kvs_store_t *store, int engine, kvs_mutation_t operation,
                     const char *key, const char *value) {
	/* All four backends share the same return contract. Allocate before FLUSH
	 * replaces anything so failure leaves the original store available. */
#define MUTATE(number, type, prefix, member)                                     \
	case number: {                                                              \
		type *target = &store->member;                                          \
		if (operation == KVS_CLEAR) {                                           \
			type empty = {0};                                                   \
			if (prefix##_create(&empty)) return -1;                             \
			prefix##_destroy(target);                                           \
			*target = empty;                                                    \
			return 0;                                                           \
		}                                                                       \
		if (operation == KVS_DELETE) return prefix##_del(target, key);   \
		if (operation == KVS_UPDATE)                                            \
			return prefix##_mod(target, key, value);             \
		int result = prefix##_set(target, key, value);           \
		if (operation == KVS_UPSERT && result == 1)                             \
			return prefix##_mod(target, key, value);             \
		return result;                                                         \
	}
	switch (engine) {
		MUTATE(KVS_FORMAT_ENGINE_ARRAY, kvs_array_t, kvs_array, array)
		MUTATE(KVS_FORMAT_ENGINE_RBTREE, kvs_rbtree_t, kvs_rbtree, rbtree)
		MUTATE(KVS_FORMAT_ENGINE_HASH, kvs_hash_t, kvs_hash, hash)
		MUTATE(KVS_FORMAT_ENGINE_SKIPTABLE, kvs_skiptable_t, kvs_skiplist, skiptable)
	default:
		return -1;
	}
#undef MUTATE
}
