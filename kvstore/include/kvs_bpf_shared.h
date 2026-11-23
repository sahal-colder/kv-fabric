/*
 * 用户态与 eBPF 共享的 Sockhash 键布局。
 * 键按值传递，程序不共享用户态指针和锁。
 */
#ifndef KVS_BPF_SHARED_H
#define KVS_BPF_SHARED_H

#include <linux/types.h>

struct kvs_socket_key {
	__u32 local_ip, remote_ip;
	__u32 local_port, remote_port;
};

#endif
