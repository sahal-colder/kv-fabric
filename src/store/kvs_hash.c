/*
 * 索引层。哈希桶保存字符串条目。
 * 表拥有桶和键值。调用方持 Store 锁，随后才可调用分配器。
 */



#include <stdint.h>
#include <string.h>
#include <stdlib.h>


#include "kvs_store.h"
#include "kvs_allocator.h"


static int hash_slot(const char *key, int slots) {
	uint32_t hash = 2166136261u;
	for (const unsigned char *byte = (const unsigned char *)key; *byte; byte++)
		hash = (hash ^ *byte) * 16777619u;
	return (int)(hash % (unsigned)slots);
}

static hashnode_t *create_node(const char *key, const char *value) {
	hashnode_t *node = (hashnode_t*)kvs_malloc(sizeof(hashnode_t));
	if (!node) return NULL;
	
#if ENABLE_KEY_POINTER
	char *kcopy = kvs_malloc(strlen(key) + 1);
	if (kcopy == NULL) {
		kvs_free(node);
		return NULL;
	}
	memcpy(kcopy, key, strlen(key) + 1);

	node->key = kcopy;

	char *kvalue = kvs_malloc(strlen(value) + 1);
	if (kvalue == NULL) { 
		kvs_free(kcopy);
		kvs_free(node);
		return NULL;
	}
	memcpy(kvalue, value, strlen(value) + 1);

	node->value = kvalue;
	
#else
	strncpy(node->key, key, MAX_KEY_LEN);
	strncpy(node->value, value, MAX_VALUE_LEN);
#endif
	node->next = NULL;

	return node;
}

static void _destroy_node(hashnode_t *node) {
#if ENABLE_KEY_POINTER
	kvs_free(node->key);
	kvs_free(node->value);
#endif
	kvs_free(node);
}


//
int kvs_hash_create(kvs_hash_t *hash) {

	if (!hash) return -1;

	hash->nodes = (hashnode_t**)kvs_malloc(sizeof(hashnode_t*) * MAX_TABLE_SIZE);
	if (!hash->nodes) return -1;
	memset(hash->nodes, 0, sizeof(hashnode_t*) * MAX_TABLE_SIZE);

	hash->max_slots = MAX_TABLE_SIZE;
	hash->count = 0; 

	return 0;
}

// 
void kvs_hash_destroy(kvs_hash_t *hash) {

	if (!hash || !hash->nodes) return;

	int i = 0;
	for (i = 0;i < hash->max_slots;i ++) {
		hashnode_t *node = hash->nodes[i];

		while (node != NULL) {

			hashnode_t *tmp = node;
			node = node->next;
			hash->nodes[i] = node;
			
			_destroy_node(tmp);
			
		}
	}

	kvs_free(hash->nodes);
	hash->nodes = NULL;
	hash->max_slots = 0;
	hash->count = 0;
	
}

// 5 + 2

// mp
int kvs_hash_set(kvs_hash_t *hash, const char *key, const char *value) {

	if (!hash || !hash->nodes || !key || !value) return -1;

	int idx = hash_slot(key, hash->max_slots);

	hashnode_t *node = hash->nodes[idx];
	while (node != NULL) {
		if (strcmp(node->key, key) == 0) { // exist
			return 1;
		}
		node = node->next;
	}

	hashnode_t *new_node = create_node(key, value);
	if (!new_node) return -2;
	new_node->next = hash->nodes[idx];
	hash->nodes[idx] = new_node;
	
	hash->count ++;

	return 0;
}


char * kvs_hash_get(kvs_hash_t *hash, const char *key) {

	if (!hash || !hash->nodes || !key) return NULL;

	int idx = hash_slot(key, hash->max_slots);

	hashnode_t *node = hash->nodes[idx];

	while (node != NULL) {

		if (strcmp(node->key, key) == 0) {
			return node->value;
		}

		node = node->next;
	}

	return NULL;

}


int kvs_hash_mod(kvs_hash_t *hash, const char *key, const char *value) {

	if (!hash || !hash->nodes || !key || !value) return -1;

	int idx = hash_slot(key, hash->max_slots);

	hashnode_t *node = hash->nodes[idx];

	while (node != NULL) {

		if (strcmp(node->key, key) == 0) {
			break;
		}

		node = node->next;
	}

	if (node == NULL) {
		return 1;
	}

	char *kvalue = kvs_malloc(strlen(value) + 1);
	if (kvalue == NULL) return -2;
	memcpy(kvalue, value, strlen(value) + 1);

	kvs_free(node->value);
	node->value = kvalue;
	
	return 0;
}

int kvs_hash_del(kvs_hash_t *hash, const char *key) {
	if (!hash || !hash->nodes || !key) return -1;

	int idx = hash_slot(key, hash->max_slots);

	hashnode_t **link = &hash->nodes[idx];
	while (*link != NULL && strcmp((*link)->key, key) != 0) {
		link = &(*link)->next;
	}
	if (*link == NULL) return 1;

	hashnode_t *node = *link;
	*link = node->next;
	_destroy_node(node);
	
	hash->count --;

	return 0;
}
