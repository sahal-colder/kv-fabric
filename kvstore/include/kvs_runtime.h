/*
 * 进程生命周期接口。标志属于服务进程，用于网络与后台线程退出。
 * 原子读取停止标志，不持存储或网络锁。
 */
#ifndef KVS_RUNTIME_H
#define KVS_RUNTIME_H

#include <stdatomic.h>

extern atomic_bool kvs_shutdown_requested;

#endif
