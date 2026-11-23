/*
 * 进程入口。依次建立配置、分配器、存储、复制和网络，退出时逆序关闭。
 * main 拥有组件寿命。组件自行加锁，信号处理只写无锁原子标志。
 */
#include "kvs_persistence.h"
#include "kvs_allocator.h"
#include "kvs_engine.h"
#include "kvs_master.h"
#include "kvs_network.h"
#include "kvs_replication.h"
#include "kvs_slave.h"
#include "kvs_sync.h"
#include "kvs_config.h"
#include "kvs_runtime.h"
#include <signal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "signal handler requires lock-free atomics");
atomic_bool kvs_shutdown_requested;

static void stop_server(int signal_number) {
	(void)signal_number;
	atomic_store_explicit(&kvs_shutdown_requested, 1, memory_order_relaxed);
}

static int start_network(const kvs_config_t *config) {
	if (!strcmp(config->network, "reactor"))
		return reactor_start_protocol(config->port);
#if KVS_ENABLE_PROACTOR
	if (!strcmp(config->network, "proactor"))
		return proactor_start_protocol(config->port);
#endif
#if KVS_ENABLE_NTYCO
	if (!strcmp(config->network, "ntyco"))
		return ntyco_start_protocol(config->port);
#endif
	fprintf(stderr, "network backend was not compiled in\n");
	return -1;
}

int main(int argc, char **argv) {
	kvs_config_t config;
	char error[KVS_CONFIG_ERROR_MAX] = {0};
	int result = -1;
	if (argc == 2 && !strcmp(argv[1], "--help")) {
		puts("usage: kvstore [port [engine]] [--config file] [--directive value] "
		     "[--check-config|--print-config]");
		return 0;
	}
	kvs_config_defaults(&config);
	if (kvs_config_cli(&config, argc, argv, error)) {
		fprintf(stderr, "config: %s\n", error);
		return 1;
	}
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--print-config")) {
			kvs_config_print(&config, stdout);
			return 0;
		}
		if (!strcmp(argv[i], "--check-config")) {
			puts("configuration OK");
			return 0;
		}
	}
	if (kvs_config_apply(&config, error) || kvs_resp_select_engine(config.engine)) {
		fprintf(stderr, "config: %s\n", error);
		return 1;
	}
	struct sigaction action = {.sa_handler = stop_server};
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGXFSZ, SIG_IGN);
#if KVS_ENABLE_NTYCO
	/* The bundled coroutine accept hook does not expose cancellation. */
	if (!strcmp(config.network, "ntyco")) {
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
	}
#endif
	if (kvs_allocator_init())
		return 1;
	if (kvs_store_create(&kvs_store))
		goto allocator;
	if (kvs_persistence_start()) {
		fprintf(stderr, "persistence startup failed\n");
		goto engine;
	}
	if (kvs_bpf_start()) {
		perror("eBPF initialization");
		goto persistence;
	}
	if (kvs_master_start()) {
		perror("replication listener");
		goto bpf;
	}
	if (config.replication.role == KVS_ROLE_SLAVE && kvs_slave_init(&config.replication))
		goto master;
	fprintf(stderr, "kvstore listening on %s:%u, engine=%s, transport=%s\n", config.bind,
	        config.port, config.engine, config.replication.transport ? "rdma" : "tcp");
	result = start_network(&config);
	kvs_slave_stop();
master:
	kvs_master_stop();
bpf:
	kvs_bpf_stop();
persistence:
	if (kvs_persistence_stop())
		result = -1;
engine:
	kvs_store_destroy(&kvs_store);
allocator:
	kvs_allocator_destroy();
	return result ? 1 : 0;
}
