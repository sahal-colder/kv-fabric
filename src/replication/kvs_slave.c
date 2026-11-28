/*
 * 复制层。接收、安装全量状态并顺序应用后续变更。
 * 从节点线程拥有复制连接，状态锁只保护连接和停机标志。
 */
#include "kvs_slave.h"
#include "kvs_persistence.h"
#include "kvs_allocator.h"
#include "kvs_format.h"
#include "kvs_sync.h"
#include "kvs_engine.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static int started, stopping, connected;
static kvs_transport_t master = {.fd = -1};

static int apply_update(kvs_sync_frame_t *frame) {
	kvs_resp_arg_t args[KVS_RESP_MAX_ARGS];
	size_t offset = 0, length;
	int argc;
	char command[16];
	if (kvs_format_parse_resp_record(frame->data, frame->length, &offset, args, &argc, &length) !=
	        1 ||
	    offset != frame->length || kvs_format_arg_to_string(&args[0], command, sizeof(command)) ||
	    kvs_format_command_engine(command) < 0)
		return -1;
	int flush = strstr(command, "FLUSH") != NULL;
	int remove = strstr(command, "DEL") != NULL;
	if (argc != (flush ? 1 : remove ? 2 : 3))
		return -1;
	/* Engines are string based. Reject embedded NUL instead of truncating a frame. */
	for (int i = 1; i < argc; i++) {
		if (memchr(args[i].data, 0, args[i].length))
			return -1;
		((char *)args[i].data)[args[i].length] = 0;
	}
	return kvs_engine_replay(command, argc > 1 ? (char *)args[1].data : NULL,
	                         argc > 2 ? (char *)args[2].data : NULL);
}

static int receive_updates(kvs_transport_t *io) {
	kvs_sync_frame_t frame;
	uint64_t sequence;
	if (kvs_sync_receive(io, &frame))
		return -1;
	int result =
	    frame.type == KVS_SYNC_SNAPSHOT ? kvs_snapshot_import(frame.data, frame.length) : -1;
	sequence = frame.sequence;
	kvs_free(frame.data);
	if (result || kvs_sync_ack(io, sequence))
		return -1;
	pthread_mutex_lock(&mutex);
	if (!stopping)
		connected = 1;
	pthread_mutex_unlock(&mutex);
	for (;;) {
		if (kvs_sync_receive(io, &frame))
			return -1;
		result = -1;
		if (frame.type == KVS_SYNC_UPDATE && sequence != UINT64_MAX &&
		    frame.sequence == sequence + 1) {
			result = apply_update(&frame);
			if (!result)
				sequence = frame.sequence;
		} else if (frame.type == KVS_SYNC_PING && frame.sequence == sequence) {
			result = kvs_sync_ack(io, sequence);
		}
		kvs_free(frame.data);
		if (result)
			return -1;
	}
}

static void *manage_slave(void *unused) {
	const kvs_replication_config_t *c = kvs_replication_config();
	(void)unused;
	for (;;) {
		kvs_transport_t io = {.fd = -1};
			pthread_mutex_lock(&mutex);
		int stop = stopping;
		pthread_mutex_unlock(&mutex);
		if (stop)
			break;
		if (!kvs_transport_connect(&io, c->master_host, c->master_port,
		                           c->transport == KVS_SYNC_RDMA, c->timeout_ms)) {
			pthread_mutex_lock(&mutex);
			master = io;
			if (stopping)
				kvs_transport_shutdown(&master);
			pthread_mutex_unlock(&mutex);
			if ((io.rdma || !kvs_bpf_add(io.fd)) &&
			    !kvs_transport_authenticate(&io, c->secret, 0, c->timeout_ms))
				(void)receive_updates(&io);
		}
		pthread_mutex_lock(&mutex);
		connected = 0;
		master.fd = -1;
		if (!io.rdma && io.fd >= 0)
			kvs_bpf_remove(io.fd);
		kvs_transport_close(&io);
		if (!stopping) {
			struct timespec until;
			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_sec += c->reconnect_ms / 1000;
			until.tv_nsec += (long)(c->reconnect_ms % 1000) * 1000000;
			if (until.tv_nsec >= 1000000000) {
				until.tv_sec++;
				until.tv_nsec -= 1000000000;
			}
			pthread_cond_timedwait(&changed, &mutex, &until);
		}
		pthread_mutex_unlock(&mutex);
	}
	return NULL;
}

int kvs_slave_init(kvs_replication_config_t *config) {
	if (!config || config->role != KVS_ROLE_SLAVE || started)
		return -1;
	stopping = connected = 0;
	if (pthread_create(&thread, NULL, manage_slave, NULL))
		return -1;
	started = 1;
	return 0;
}

void kvs_slave_stop(void) {
	if (!started)
		return;
	pthread_mutex_lock(&mutex);
	stopping = 1;
	kvs_transport_shutdown(&master);
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&mutex);
	pthread_join(thread, NULL);
	started = 0;
}

int kvs_slave_is_connected(void) {
	pthread_mutex_lock(&mutex);
	int result = connected;
	pthread_mutex_unlock(&mutex);
	return result;
}
