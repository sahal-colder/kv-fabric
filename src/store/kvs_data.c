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

/* Allocate only the batch's distinct keys in a private index. The caller owns
 * the Store write lock. Once preparation succeeds, merging transfers ownership
 * without allocations; a failed preparation leaves the live store untouched. */
#include "kvs_allocator.h"
#include <string.h>
extern rbtree_node *rbtree_search(rbtree *, const char *);
extern void rbtree_insert(rbtree *, rbtree_node *);

static void merge_tree(rbtree *dst, rbtree_node *node, rbtree_node *nil) {
    if (node == nil)
        return;
    rbtree_node *left = node->left, *right = node->right;
    merge_tree(dst, left, nil);
    merge_tree(dst, right, nil);
    rbtree_node *old = rbtree_search(dst, node->key);
    if (old == dst->nil) {
        rbtree_insert(dst, node);
    } else {
        kvs_free(old->value);
        old->value = node->value;
        kvs_free(node->key);
        kvs_free(node);
    }
}

static void merge_skiplist(kvs_skiptable_t *dst, kvs_skiptable_t *src) {
    skiptablenode_t *next[MAX_LEVEL + 1];
    for (int level = 0; level <= MAX_LEVEL; ++level)
        next[level] = src->header->forward[level];
    while (next[0]) {
        skiptablenode_t *node = next[0], *update[MAX_LEVEL + 1];
        skiptablenode_t *at = dst->header;
        for (int level = dst->level; level >= 0; --level) {
            while (at->forward[level] && strcmp(at->forward[level]->key, node->key) < 0)
                at = at->forward[level];
            update[level] = at;
        }
        for (int level = dst->level + 1; level <= MAX_LEVEL; ++level)
            update[level] = dst->header;
        skiptablenode_t *old = update[0]->forward[0];
        int exists = old && !strcmp(old->key, node->key);
        for (int level = 0; level <= src->level; ++level) {
            if (next[level] != node)
                continue;
            next[level] = node->forward[level]; /* before transferring its links */
            if (!exists) {
                node->forward[level] = update[level]->forward[level];
                update[level]->forward[level] = node;
                if (level > dst->level)
                    dst->level = level;
            }
        }
        if (exists) {
            kvs_free(old->value);
            old->value = node->value;
            kvs_free(node->key);
            kvs_free(node->forward);
            kvs_free(node);
        }
    }
    memset(src->header->forward, 0, (MAX_LEVEL + 1) * sizeof(*src->header->forward));
}

int kvs_store_mset(kvs_store_t *dst, int engine, const char *const *keys,
                   const char *const *values, size_t count, int only_missing) {
    if (!dst || !keys || !values || !count || count > 64)
        return -1;
    for (size_t i = 0; i < count; ++i) {
        if (!keys[i] || !values[i])
            return -1;
        if (only_missing && kvs_store_get(dst, engine, keys[i]))
            return 1;
    }
    kvs_store_t staged = {0};
    int result;
    switch (engine) {
    case KVS_FORMAT_ENGINE_ARRAY: result = kvs_array_create(&staged.array); break;
    case KVS_FORMAT_ENGINE_RBTREE: result = kvs_rbtree_create(&staged.rbtree); break;
    case KVS_FORMAT_ENGINE_HASH: result = kvs_hash_create(&staged.hash); break;
    case KVS_FORMAT_ENGINE_SKIPTABLE: result = kvs_skiplist_create(&staged.skiptable); break;
    default: return -1;
    }
    if (result)
        return -1;
    for (size_t i = 0; i < count; ++i) {
        if (kvs_store_mutate(&staged, engine, KVS_UPSERT, keys[i], values[i])) {
            result = -1;
            goto done;
        }
    }
    if (engine == KVS_FORMAT_ENGINE_ARRAY) {
        int needed = 0;
        for (int i = 0; i < staged.array.total; ++i)
            needed += kvs_array_get(&dst->array, staged.array.table[i].key) == NULL;
        if (needed > KVS_ARRAY_SIZE - kvs_store_count(dst, engine)) {
            result = -1;
            goto done;
        }
        for (int i = 0; i < staged.array.total; ++i) {
            kvs_array_item_t *item = &staged.array.table[i];
            int slot = 0;
            while (slot < dst->array.total &&
                   (!dst->array.table[slot].key || strcmp(dst->array.table[slot].key, item->key)))
                ++slot;
            if (slot < dst->array.total) {
                kvs_free(dst->array.table[slot].value);
                dst->array.table[slot].value = item->value;
                item->value = NULL;
            } else {
                slot = 0;
                while (dst->array.table[slot].key)
                    ++slot;
                dst->array.table[slot] = *item;
                *item = (kvs_array_item_t){0};
                if (slot >= dst->array.total)
                    dst->array.total = slot + 1;
            }
        }
    } else if (engine == KVS_FORMAT_ENGINE_RBTREE) {
        merge_tree(&dst->rbtree, staged.rbtree.root, staged.rbtree.nil);
        staged.rbtree.root = staged.rbtree.nil;
    } else if (engine == KVS_FORMAT_ENGINE_HASH) {
        /* Both indexes use the same immutable bucket count/hash function. */
        if (dst->hash.max_slots != staged.hash.max_slots) {
            result = -1;
            goto done;
        }
        for (int i = 0; i < staged.hash.max_slots; ++i) {
            hashnode_t *node = staged.hash.nodes[i];
            staged.hash.nodes[i] = NULL;
            while (node) {
                hashnode_t *next = node->next, *old = dst->hash.nodes[i];
                while (old && strcmp(old->key, node->key))
                    old = old->next;
                if (old) {
                    kvs_free(old->value);
                    old->value = node->value;
                    kvs_free(node->key);
                    kvs_free(node);
                } else {
                    node->next = dst->hash.nodes[i];
                    dst->hash.nodes[i] = node;
                    ++dst->hash.count;
                }
                node = next;
            }
        }
    } else {
        merge_skiplist(&dst->skiptable, &staged.skiptable);
    }
done:
    kvs_store_destroy(&staged);
    return result;
}
