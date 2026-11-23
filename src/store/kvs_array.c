/*
 * 索引层。用连续槽位存放字符串键和值。
 * 表拥有键值分配。调用方持 Store 锁，随后才可调用分配器。
 */



#include "kvs_store.h"
#include "kvs_allocator.h"

#include <string.h>


// singleton


int kvs_array_create(kvs_array_t *inst) {

	if (!inst) return -1;
	if (inst->table) return -1;
	inst->table = kvs_malloc(KVS_ARRAY_SIZE * sizeof(kvs_array_item_t));
	if (!inst->table) {
		return -1;
	}
	memset(inst->table, 0, KVS_ARRAY_SIZE * sizeof(kvs_array_item_t));

	inst->total = 0;

	return 0;
}

void kvs_array_destroy(kvs_array_t *inst) {

	if (!inst) return ;

	if (!inst->table) return;

	for (int i = 0; i < inst->total; i ++) {
		kvs_free(inst->table[i].key);
		kvs_free(inst->table[i].value);
	}
	kvs_free(inst->table);
	inst->table = NULL;
	inst->total = 0;

}


/*
 * @return: <0, error; =0, success; >0, exist
 */

int kvs_array_set(kvs_array_t *inst, const char *key, const char *value) {

	if (inst == NULL || key == NULL || value == NULL) return -1;
	if (inst->table == NULL) return -1;

	char *str = kvs_array_get(inst, key);
	if (str) {
		return 1; // 
	}

	int slot = 0;
	while (slot < inst->total && inst->table[slot].key != NULL) {
		slot ++;
	}
	if (slot == KVS_ARRAY_SIZE) return -1;

	char *kcopy = kvs_malloc(strlen(key) + 1);
	if (kcopy == NULL) return -2;
	memcpy(kcopy, key, strlen(key) + 1);

	char *kvalue = kvs_malloc(strlen(value) + 1);
	if (kvalue == NULL) {
		kvs_free(kcopy);
		return -2;
	}
	memcpy(kvalue, value, strlen(value) + 1);

	inst->table[slot].key = kcopy;
	inst->table[slot].value = kvalue;
	if (slot == inst->total) {
		inst->total ++;
	}

	return 0;
}

char* kvs_array_get(kvs_array_t *inst, const char *key) {

	if (inst == NULL || inst->table == NULL || key == NULL) return NULL;

	int i = 0;
	for (i = 0;i < inst->total;i ++) {
		if (inst->table[i].key == NULL) {
			continue;
		}

		if (strcmp(inst->table[i].key, key) == 0) {
			return inst->table[i].value;
		}
	}

	return NULL;
}


/*
 * @return < 0, error;  =0,  success; >0, no exist
 */

int kvs_array_del(kvs_array_t *inst, const char *key) {

	if (inst == NULL || inst->table == NULL || key == NULL) return -1;

	int i = 0;
	for (i = 0;i < inst->total;i ++) {
		if (inst->table[i].key == NULL) {
			continue;
		}

		if (strcmp(inst->table[i].key, key) == 0) {

			kvs_free(inst->table[i].key);
			inst->table[i].key = NULL;

			kvs_free(inst->table[i].value);
			inst->table[i].value = NULL;
			if (inst->total - 1 == i) {
				while (inst->total > 0 && inst->table[inst->total - 1].key == NULL) {
					inst->total --;
				}
			}
			

			return 0;
		}
	}

	return 1;
}


/*
 * @return : < 0, error; =0, success; >0, no exist 
 */

int kvs_array_mod(kvs_array_t *inst, const char *key, const char *value) {

	if (inst == NULL || inst->table == NULL || key == NULL || value == NULL) return -1;
	

	int i = 0;
	for (i = 0;i < inst->total;i ++) {

		if (inst->table[i].key == NULL) {
			continue;
		}

		if (strcmp(inst->table[i].key, key) == 0) {

			char *kvalue = kvs_malloc(strlen(value) + 1);
			if (kvalue == NULL) return -2;
			memcpy(kvalue, value, strlen(value) + 1);

			kvs_free(inst->table[i].value);
			inst->table[i].value = kvalue;

			return 0;
		}

	}

	return 1;
}
