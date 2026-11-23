/*
 * 索引层。多级链表维护有序字符串键。
 * 表拥有节点和键值。调用方持 Store 锁，随后才可调用分配器。
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>


#include "kvs_store.h"
#include "kvs_allocator.h"



static skiptablenode_t* create_node(int level, const char *key, const char *value) {
    skiptablenode_t* newNode = (skiptablenode_t*)kvs_calloc(1, sizeof(skiptablenode_t));
	if (newNode == NULL) return NULL;
    
    //newNode->key = key;
    if(key != NULL){
	   	newNode->key = (char*)kvs_malloc(strlen(key)+1);
		if (newNode->key == NULL) {
			kvs_free(newNode);
			return NULL;
		}
	    memcpy(newNode->key,key,strlen(key)+1);
    }
    
    //newNode->value = value;
     if(value != NULL){
		newNode->value = (char*)kvs_malloc(strlen(value)+1);
		if (newNode->value == NULL) {
			kvs_free(newNode->key);
			kvs_free(newNode);
			return NULL;
		}
		memcpy(newNode->value,value,strlen(value)+1);
    }

    
    newNode->forward = (skiptablenode_t**)kvs_calloc(level + 1, sizeof(skiptablenode_t*));
	if (newNode->forward == NULL) {
		kvs_free(newNode->key);
		kvs_free(newNode->value);
		kvs_free(newNode);
		return NULL;
	}
    
    return newNode;
}

int kvs_skiplist_create(kvs_skiptable_t* skipList) {

	if(skipList == NULL)return -1;

    //SkipList* skipList = (SkipList*)kvs_malloc(sizeof(SkipList));
    skipList->level = 0;
    
    skipList->header = create_node(MAX_LEVEL, NULL, NULL);
	if (skipList->header == NULL) return -1;
    
   return 0; 
}

static int random_level(void) {
   int level = 0;
   while (level < MAX_LEVEL && rand() < RAND_MAX / 2)
      level++;
   return level;
}

int kvs_skiplist_set(kvs_skiptable_t* skipList, const char *key, const char *value) {
	if(skipList == NULL || skipList->header == NULL || key == NULL || value == NULL) return -1;

   skiptablenode_t* update[MAX_LEVEL + 1];
   skiptablenode_t* current = skipList->header;

   for (int i = skipList->level; i >= 0; --i) {
      while (current->forward[i] != NULL && strcmp(current->forward[i]->key,key) < 0)//strcmp(z->key, x->key) < 0
         current = current->forward[i];
      update[i] = current;
   }

   current = current->forward[0];

   if (current == NULL || (strcmp(current->key,key) != 0)) {
      int level = random_level();

      skiptablenode_t *newNode = create_node(level, key, value);
      if (!newNode) return -1;

      if (level > skipList->level) {
         for (int i = skipList->level + 1; i <= level; ++i)
            update[i] = skipList->header;
         skipList->level = level;
      }

      for (int i = 0; i <= level; ++i) {
         newNode->forward[i] = update[i]->forward[i];
         update[i]->forward[i] = newNode;
      }
      
     // printf("Inserted key %s\n", key);
      
      return 0;
   } else {
       //printf("Key %s already exists\n", key);
       return 1;
   }
}

skiptablenode_t*  kvs_skiplist_get(kvs_skiptable_t* skipList, const char *key) {
    if (skipList == NULL || skipList->header == NULL || key == NULL) return NULL;

    skiptablenode_t* current = skipList->header;
	int i = 0;
    for (i = skipList->level; i >= 0; --i) {
        while (current->forward[i] != NULL && strcmp(current->forward[i]->key,key) < 0)
            current = current->forward[i];
    }

    current = current->forward[0];

    if(current != NULL && strcmp(current->key,key) == 0){
        //printf("Key %s found with value %s\n", key, current->value);
        return current;
    }else{
        //printf("Key %s not found\n", key);
        return NULL;
    }
}



int kvs_skiplist_del(kvs_skiptable_t* skipList, const char *key) {

   	if (skipList == NULL || skipList->header == NULL || key == NULL) return -1;
	skiptablenode_t* latter[MAX_LEVEL + 1] = {0};
   	skiptablenode_t* current = skipList->header;

   	for (int i = skipList->level; i >= 0; --i) {
   		while (current->forward[i] != NULL && strcmp(current->forward[i]->key,key) < 0){//strcmp(z->key, x->key) < 0
        	current = current->forward[i];
      	}
      	latter[i] = current;
   	}
	
    current = current->forward[0];

	if(current != NULL && strcmp(current->key,key) == 0){

	    //printf("Key %s found with value %s\n", key, current->value);
	    for (int i = skipList->level; i >= 0; --i){
	    	if(latter[i]->forward[i] != current){
				continue;
	    	}

	    	latter[i]->forward[i] = current->forward[i];
		}
		
		kvs_free(current->key);
		kvs_free(current->value);
		kvs_free(current->forward);
		kvs_free(current);

		while (skipList->level > 0 && skipList->header->forward[skipList->level] == NULL) {
            skipList->level--;
        }

        return 0;
	}
	    else{

	    
	        //printf("Key %s not found\n", key);
	        return 1;
	    }
}



int kvs_skiplist_mod(kvs_skiptable_t* skipList, const char *key,const char *value) {

	if(skipList == NULL || key == NULL ||  value == NULL)return -1;
	
	skiptablenode_t* current = kvs_skiplist_get(skipList,key);
	if(current == NULL)return 1;

	char *new_value = (char*)kvs_malloc(strlen(value) + 1);
	if (new_value == NULL) return -1;
	memcpy(new_value, value, strlen(value) + 1);

	kvs_free(current->value);
	current->value = new_value;

	return 0;
}

void kvs_skiplist_destroy(kvs_skiptable_t *list) {
	skiptablenode_t *node = list->header;
	while (node) {
		skiptablenode_t *next = node->forward[0];
		kvs_free(node->key);
		kvs_free(node->value);
		kvs_free(node->forward);
		kvs_free(node);
		node = next;
	}
	*list = (kvs_skiptable_t){0};
}
