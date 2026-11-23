/*
 * 通用存储层。包含四种索引以及进程共享 Store 的生命周期。
 * 各索引拥有键和值。运行期先取得 Store 读写锁再访问索引。
 */
#ifndef KVS_STORE_H
#define KVS_STORE_H

#include "kvs_mutation.h"

#define ENABLE_ARRAY		1
#define ENABLE_RBTREE		1
#define ENABLE_HASH			1
#define ENABLE_SKIPTABLE	1

#if ENABLE_ARRAY

typedef struct kvs_array_item_s {
	char *key;
	char *value;
} kvs_array_item_t;

#define KVS_ARRAY_SIZE		1024

typedef struct kvs_array_s {
	kvs_array_item_t *table;
	int total;
} kvs_array_t;

int kvs_array_create(kvs_array_t *inst);
void kvs_array_destroy(kvs_array_t *inst);

int kvs_array_set(kvs_array_t *inst, const char *key, const char *value);
char* kvs_array_get(kvs_array_t *inst, const char *key);
int kvs_array_del(kvs_array_t *inst, const char *key);
int kvs_array_mod(kvs_array_t *inst, const char *key, const char *value);


#endif


#if ENABLE_RBTREE

#define RED				1
#define BLACK 			2

#define ENABLE_KEY_CHAR		1

#if ENABLE_KEY_CHAR
typedef char* KEY_TYPE;
#else
typedef int KEY_TYPE; // key
#endif

typedef struct _rbtree_node {
	unsigned char color;
	struct _rbtree_node *right;
	struct _rbtree_node *left;
	struct _rbtree_node *parent;
	KEY_TYPE key;
	void *value;
} rbtree_node;

typedef struct _rbtree {
	rbtree_node *root;
	rbtree_node *nil;
} rbtree;


typedef struct _rbtree kvs_rbtree_t;

int kvs_rbtree_create(kvs_rbtree_t *inst);
void kvs_rbtree_destroy(kvs_rbtree_t *inst);
int kvs_rbtree_set(kvs_rbtree_t *inst, const char *key, const char *value);
char* kvs_rbtree_get(kvs_rbtree_t *inst, const char *key);
int kvs_rbtree_del(kvs_rbtree_t *inst, const char *key);
int kvs_rbtree_mod(kvs_rbtree_t *inst, const char *key, const char *value);



#endif


#if ENABLE_HASH

#define MAX_KEY_LEN	128
#define MAX_VALUE_LEN	512
#define MAX_TABLE_SIZE	1024

#define ENABLE_KEY_POINTER	1


typedef struct hashnode_s {
#if ENABLE_KEY_POINTER
	char *key;
	char *value;
#else
	char key[MAX_KEY_LEN];
	char value[MAX_VALUE_LEN];
#endif
	struct hashnode_s *next;

} hashnode_t;


typedef struct hashtable_s {

	hashnode_t **nodes;

	int max_slots;
	int count;

} hashtable_t;

typedef struct hashtable_s kvs_hash_t;


int kvs_hash_create(kvs_hash_t *hash);
void kvs_hash_destroy(kvs_hash_t *hash);
int kvs_hash_set(hashtable_t *hash, const char *key, const char *value);
char * kvs_hash_get(kvs_hash_t *hash, const char *key);
int kvs_hash_mod(kvs_hash_t *hash, const char *key, const char *value);
int kvs_hash_del(kvs_hash_t *hash, const char *key);


#endif

#if ENABLE_SKIPTABLE

#define MAX_LEVEL 6

typedef struct Node {
    char* key;
    char* value;
    struct Node** forward;
} skiptablenode_t;

typedef struct SkipList {
    int level;			/* 当前跳表最大层数 */
    skiptablenode_t* header;		/* 头节点 */
} kvs_skiptable_t;

int kvs_skiplist_create(kvs_skiptable_t* skipList);
void kvs_skiplist_destroy(kvs_skiptable_t* skipList);
int kvs_skiplist_set(kvs_skiptable_t* skipList, const char *key, const char *value);
skiptablenode_t* kvs_skiplist_get(kvs_skiptable_t* skipList, const char *key);
int kvs_skiplist_del(kvs_skiptable_t* skipList, const char *key);
int kvs_skiplist_mod(kvs_skiptable_t* skipList, const char *key, const char *value);
#endif

typedef struct {
	kvs_array_t array;
	kvs_rbtree_t rbtree;
	kvs_hash_t hash;
	kvs_skiptable_t skiptable;
} kvs_store_t;

typedef enum {
	KVS_INSERT,
	KVS_UPDATE,
	KVS_UPSERT,
	KVS_DELETE,
	KVS_CLEAR
} kvs_mutation_t;

void kvs_store_read_lock(void);
void kvs_store_write_lock(void);
void kvs_store_unlock(void);

/* One live store. Other instances belong to a single recovery worker. */
extern kvs_store_t kvs_store;

int kvs_store_create(kvs_store_t *store);
void kvs_store_destroy(kvs_store_t *store);
/* Caller holds the write lock. After the swap, staged owns the old data. */
void kvs_store_swap(kvs_store_t *staged);
/* The value is borrowed until the store is next mutated. */
const char *kvs_store_get(kvs_store_t *store, int engine, const char *key);
int kvs_store_count(kvs_store_t *store, int engine);
/* 0 changed, 1 condition unmet (key exists or is missing), negative on error. */
int kvs_store_mutate(kvs_store_t *store, int engine, kvs_mutation_t operation,
                     const char *key, const char *value);

#endif
