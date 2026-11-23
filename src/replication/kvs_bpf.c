/*
 * 复制加速层。加载并管理 eBPF 程序及 Sockhash 链接。
 * 进程持有 BPF 对象与 map 描述符，生命周期由主程序控制。
 */
#include "kvs_transport.h"
#include "kvs_replication.h"

#include <errno.h>
#include <stdio.h>

#if KVS_ENABLE_BPF
#include "kvs_bpf_shared.h"
#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <unistd.h>

static int map_fd = -1;

static int socket_key(int fd, struct kvs_socket_key *key) {
	struct sockaddr_in local, remote;
	socklen_t n = sizeof(local);
	if (getsockname(fd, (struct sockaddr *)&local, &n))
		return -1;
	n = sizeof(remote);
	if (getpeername(fd, (struct sockaddr *)&remote, &n) || local.sin_family != AF_INET ||
	    remote.sin_family != AF_INET)
		return -1;
	*key = (struct kvs_socket_key){local.sin_addr.s_addr, remote.sin_addr.s_addr,
	                               ntohs(local.sin_port), ntohs(remote.sin_port)};
	return 0;
}

int kvs_bpf_start(void) {
	const kvs_replication_config_t *c = kvs_replication_config();
	struct bpf_object *object;
	struct bpf_map_info info = {0};
	__u32 length = sizeof(info);
	int program, result = -1;
	struct rlimit limit = {RLIM_INFINITY, RLIM_INFINITY};
	if (!c->ebpf_enabled)
		return 0;
	map_fd = bpf_obj_get(c->ebpf_map);
	if (map_fd >= 0) {
		if (bpf_obj_get_info_by_fd(map_fd, &info, &length) || info.type != BPF_MAP_TYPE_SOCKHASH ||
		    info.key_size != sizeof(struct kvs_socket_key) || info.value_size != sizeof(__u32))
			goto failed;
		return 0;
	}
	if (errno != ENOENT)
		return -1;
	(void)setrlimit(RLIMIT_MEMLOCK, &limit);
	object = bpf_object__open_file(c->ebpf_object, NULL);
	if (libbpf_get_error(object))
		return -1;
	if (bpf_object__load(object))
		goto done;
	map_fd = bpf_object__find_map_fd_by_name(object, "replication_sockets");
	program = bpf_object__find_program_by_name(object, "kvs_sync_redirect")
	              ? bpf_program__fd(bpf_object__find_program_by_name(object, "kvs_sync_redirect"))
	              : -1;
	if (map_fd < 0 || program < 0 || bpf_prog_attach(program, map_fd, BPF_SK_MSG_VERDICT, 0)) {
		map_fd = -1;
		goto done;
	}
	/* The pinned map owns the attachment. All participating processes share it. */
	if (bpf_obj_pin(map_fd, c->ebpf_map)) {
		map_fd = -1;
		if (errno == EEXIST) {
			bpf_object__close(object);
			return kvs_bpf_start();
		}
		goto done;
	}
	map_fd = dup(map_fd);
	result = map_fd < 0 ? -1 : 0;
done:
	bpf_object__close(object);
	return result;
failed:
	kvs_bpf_stop();
	return -1;
}

int kvs_bpf_add(int fd) {
	struct kvs_socket_key key;
	if (map_fd < 0)
		return 0;
	return socket_key(fd, &key) ? -1 : bpf_map_update_elem(map_fd, &key, &fd, BPF_NOEXIST);
}

void kvs_bpf_remove(int fd) {
	struct kvs_socket_key key;
	if (map_fd >= 0 && !socket_key(fd, &key))
		(void)bpf_map_delete_elem(map_fd, &key);
}

void kvs_bpf_stop(void) {
	if (map_fd >= 0)
		close(map_fd);
	map_fd = -1;
}
#else
int kvs_bpf_start(void) {
	if (!kvs_replication_config()->ebpf_enabled)
		return 0;
	errno = ENOTSUP;
	return -1;
}
void kvs_bpf_stop(void) {}
int kvs_bpf_add(int fd) {
	(void)fd;
	return 0;
}
void kvs_bpf_remove(int fd) { (void)fd; }
#endif
