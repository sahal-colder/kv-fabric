/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 内核复制层。按对端键查 Sockhash 并重定向数据。
 * 内核拥有 map 和 socket 引用，BPF 程序不进入用户态锁。
 */
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#include "kvs_bpf_shared.h"

struct {
	__uint(type, BPF_MAP_TYPE_SOCKHASH);
	__uint(max_entries, 256);
	__type(key, struct kvs_socket_key);
	__type(value, __u32);
} replication_sockets SEC(".maps");

SEC("sk_msg")
int kvs_sync_redirect(struct sk_msg_md *msg) {
	struct kvs_socket_key peer = {
	    .local_ip = msg->remote_ip4,
	    .remote_ip = msg->local_ip4,
	    .local_port = bpf_ntohl(msg->remote_port),
	    .remote_port = msg->local_port,
	};
	/* The helper sets sk_redir on success. A missing local peer keeps TCP. */
	bpf_msg_redirect_hash(msg, &replication_sockets, &peer, BPF_F_INGRESS);
	return SK_PASS;
}

char LICENSE[] SEC("license") = "GPL";
