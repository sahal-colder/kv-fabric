/*
 * 通用存储层的进程共享读写锁。
 * 锁寿命等于进程寿命。先取 Store 锁，再进入复制或分配器。
 */
#include "kvs_store.h"

#include <pthread.h>

static pthread_rwlock_t kvs_store_lock = PTHREAD_RWLOCK_INITIALIZER;

void kvs_store_read_lock(void) {
	pthread_rwlock_rdlock(&kvs_store_lock);
}

void kvs_store_write_lock(void) {
	pthread_rwlock_wrlock(&kvs_store_lock);
}

void kvs_store_unlock(void) {
	pthread_rwlock_unlock(&kvs_store_lock);
}
