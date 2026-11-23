/*
 * 复制层。生成全量同步和有序增量队列。
 * 队列持有待发送数据。先取 Store 锁，再取主节点状态锁，发送在线程中完成。
 */
#include "kvs_master.h"
#include "kvs_store.h"
#include "kvs_persistence.h"
#include "kvs_allocator.h"
#include "kvs_format.h"
#include "kvs_replication.h"
#include "kvs_slave.h"
#include "kvs_sync.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_SLAVES 64

typedef struct message {
	atomic_uint refs;
	kvs_sync_frame_t frame;
} message_t;

typedef struct pending {
	message_t *message;
	struct pending *next;
} pending_t;

typedef struct peer {
	kvs_transport_t io;
	pthread_t thread;
	int state; /* 0 unused, 1 running, 2 joinable. Protected by mutex. */
	int failed, ready, inline_hello;
	pending_t *head, *tail;
	size_t queued;
	kvs_sync_frame_t snapshot;
} peer_t;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static peer_t peers[MAX_SLAVES];
static uint64_t sequence;
static int stopping, listener_started;
static pthread_t listener_thread;
static kvs_transport_t listener = {.fd = -1};

static void release(message_t *m) {
	if (atomic_fetch_sub(&m->refs, 1) != 1)
		return;
	kvs_free(m->frame.data);
	kvs_free(m);
}

static void disconnect_peer(peer_t *p) {
	p->failed = 1;
	p->ready = 0;
	kvs_transport_shutdown(&p->io);
}

static void *send_updates(void *arg) {
	peer_t *p = arg;
	uint64_t sent = p->snapshot.sequence;
	time_t heartbeat = time(NULL);
	int result = 0;
	if (p->inline_hello)
		result = kvs_transport_write(&p->io, KVS_MASTER_HANDSHAKE_ACK,
		                             sizeof(KVS_MASTER_HANDSHAKE_ACK) - 1,
		                             kvs_replication_config()->timeout_ms);
	if (!result)
		result = kvs_sync_send(&p->io, &p->snapshot);
	kvs_free(p->snapshot.data);
	p->snapshot.data = NULL;
	if (!result)
		result = kvs_sync_expect_ack(&p->io, sent);
	pthread_mutex_lock(&mutex);
	if (!result && !p->failed)
		p->ready = 1;
	pthread_mutex_unlock(&mutex);
	while (!result) {
		pending_t *item;
		pthread_mutex_lock(&mutex);
		if (!p->head && !p->failed && !stopping) {
			struct timespec until;
			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_sec++;
			pthread_cond_timedwait(&changed, &mutex, &until);
		}
		if (p->failed || stopping) {
			pthread_mutex_unlock(&mutex);
			break;
		}
		item = p->head;
		if (item) {
			p->head = item->next;
			if (!p->head)
				p->tail = NULL;
			p->queued -= item->message->frame.length + 24;
		}
		pthread_mutex_unlock(&mutex);
		if (item) {
			result = kvs_sync_send(&p->io, &item->message->frame);
			sent = item->message->frame.sequence;
			release(item->message);
			kvs_free(item);
		}
		if (!result && time(NULL) >= heartbeat + 1) {
			kvs_sync_frame_t ping = {.type = KVS_SYNC_PING, .sequence = sent};
			result = kvs_sync_send(&p->io, &ping);
			if (!result)
				result = kvs_sync_expect_ack(&p->io, sent);
			heartbeat = time(NULL);
		}
	}
	pthread_mutex_lock(&mutex);
	disconnect_peer(p);
	if (!p->io.rdma)
		kvs_bpf_remove(p->io.fd);
	kvs_transport_close(&p->io);
	while (p->head) {
		pending_t *item = p->head;
		p->head = item->next;
		release(item->message);
		kvs_free(item);
	}
	p->queued = 0;
	p->state = 2;
	pthread_mutex_unlock(&mutex);
	return NULL;
}

/* A read lock establishes the snapshot/delta barrier without holding a lock over I/O. */
static int register_peer(kvs_transport_t io, int inline_hello) {
	char *snapshot = NULL;
	size_t length = 0;
	int result = -1;
	if (!io.rdma) {
		int flags = fcntl(io.fd, F_GETFL, 0);
		if (flags < 0 || fcntl(io.fd, F_SETFL, flags | O_NONBLOCK) < 0 || kvs_bpf_add(io.fd))
			return -1;
	}
	kvs_store_read_lock();
	if (kvs_snapshot_export_locked(&snapshot, &length, kvs_replication_config()->snapshot_bytes))
		goto done;
	pthread_mutex_lock(&mutex);
	for (size_t i = 0; i < MAX_SLAVES && !stopping; i++) {
		peer_t *p = &peers[i];
		if (p->state == 2) {
			pthread_join(p->thread, NULL);
			p->state = 0;
		}
		if (p->state)
			continue;
		memset(p, 0, sizeof(*p));
		p->io = io;
		p->state = 1;
		p->inline_hello = inline_hello;
		p->snapshot = (kvs_sync_frame_t){KVS_SYNC_SNAPSHOT, sequence, snapshot, length};
		if (pthread_create(&p->thread, NULL, send_updates, p)) {
			p->state = 0;
			break;
		}
		snapshot = NULL;
		result = 0;
		break;
	}
	pthread_mutex_unlock(&mutex);
done:
	kvs_store_unlock();
	kvs_free(snapshot);
	if (result && !io.rdma)
		kvs_bpf_remove(io.fd);
	return result;
}

int kvs_master_register_slave(int fd) {
	/* Separate descriptors let the network detach its epoll/uring state even
	 * if the worker immediately encounters a send error and closes its copy. */
	kvs_transport_t io = {.fd = fcntl(fd, F_DUPFD_CLOEXEC, 0)};
	if (io.fd < 0)
		return -1;
	if (!register_peer(io, 1))
		return 0;
	kvs_transport_close(&io);
	return -1;
}

size_t kvs_master_slave_count(void) {
	size_t count = 0;
	pthread_mutex_lock(&mutex);
	for (size_t i = 0; i < MAX_SLAVES; i++)
		if (peers[i].state == 1 && peers[i].ready)
			count++;
	pthread_mutex_unlock(&mutex);
	return count;
}

void kvs_master_resync(void) {
	if (kvs_replication_config()->role != KVS_ROLE_MASTER)
		return;
	pthread_mutex_lock(&mutex);
	for (size_t i = 0; i < MAX_SLAVES; i++) {
		if (peers[i].state == 1)
			disconnect_peer(&peers[i]);
	}
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&mutex);
}

void kvs_master_publish(const kvs_write_event_t *event) {
	const kvs_replication_config_t *c = kvs_replication_config();
	const char *args[3];
	message_t *m;
	int argc;
	if (event->source != KVS_EVENT_SOURCE_CLIENT ||
	    c->role != KVS_ROLE_MASTER)
		return;
	pthread_mutex_lock(&mutex);
	sequence++;
	size_t first = 0;
	while (first < MAX_SLAVES && (peers[first].state != 1 || peers[first].failed))
		first++;
	if (first == MAX_SLAVES) {
		pthread_mutex_unlock(&mutex);
		return;
	}
	args[0] = event->cmd;
	args[1] = event->key;
	args[2] = event->value;
	argc = strstr(event->cmd, "FLUSH") ? 1 : event->value ? 3 : 2;
	m = kvs_calloc(1, sizeof(*m));
	if (m) {
		atomic_init(&m->refs, 1);
		if (kvs_resp_build_record(args, argc, &m->frame.data, &m->frame.length) ||
		    m->frame.length > KVS_REPLICATION_MAX_COMMAND_SIZE) {
			release(m);
			m = NULL;
		}
	}
	if (m) {
		m->frame.type = KVS_SYNC_UPDATE;
		m->frame.sequence = sequence;
	}
	for (size_t i = first; i < MAX_SLAVES; i++) {
		peer_t *p = &peers[i];
		pending_t *item;
		if (p->state != 1 || p->failed)
			continue;
		if (!m || m->frame.length + 24 > c->queue_bytes - p->queued) {
			disconnect_peer(p);
			continue;
		}
		item = kvs_malloc(sizeof(*item));
		if (!item) {
			disconnect_peer(p);
			continue;
		}
		atomic_fetch_add(&m->refs, 1);
		*item = (pending_t){.message = m};
		if (p->tail)
			p->tail->next = item;
		else
			p->head = item;
		p->tail = item;
		p->queued += m->frame.length + 24;
	}
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&mutex);
	if (m)
		release(m);
}

static void *accept_slaves(void *unused) {
	const kvs_replication_config_t *c = kvs_replication_config();
	(void)unused;
	for (;;) {
		kvs_transport_t io;
		char hello[sizeof(KVS_SLAVE_HANDSHAKE) - 1];
		pthread_mutex_lock(&mutex);
		int stop = stopping;
		pthread_mutex_unlock(&mutex);
		if (stop)
			break;
		if (kvs_transport_accept(&listener, &io, 100))
			continue;
		if (!kvs_transport_read(&io, hello, sizeof(hello), c->timeout_ms) &&
		    !memcmp(hello, KVS_SLAVE_HANDSHAKE, sizeof(hello)) &&
		    !kvs_transport_write(&io, KVS_MASTER_HANDSHAKE_ACK,
		                         sizeof(KVS_MASTER_HANDSHAKE_ACK) - 1, c->timeout_ms) &&
		    !register_peer(io, 0))
			continue;
		kvs_transport_close(&io);
	}
	return NULL;
}

int kvs_master_start(void) {
	const kvs_replication_config_t *c = kvs_replication_config();
	if (c->role != KVS_ROLE_MASTER)
		return 0;
	stopping = 0;
	if (!c->listen_port)
		return 0;
	if (kvs_transport_listen(&listener, c->bind_host, c->listen_port,
	                         c->transport == KVS_SYNC_RDMA))
		return -1;
	if (pthread_create(&listener_thread, NULL, accept_slaves, NULL)) {
		kvs_transport_close(&listener);
		return -1;
	}
	listener_started = 1;
	return 0;
}

void kvs_master_stop(void) {
	pthread_mutex_lock(&mutex);
	stopping = 1;
	for (size_t i = 0; i < MAX_SLAVES; i++)
		if (peers[i].state == 1)
			disconnect_peer(&peers[i]);
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&mutex);
	if (listener_started) {
		pthread_join(listener_thread, NULL);
		listener_started = 0;
		kvs_transport_close(&listener);
	}
	for (size_t i = 0; i < MAX_SLAVES; i++) {
		pthread_mutex_lock(&mutex);
		int join = peers[i].state != 0;
		pthread_t thread = peers[i].thread;
		pthread_mutex_unlock(&mutex);
		if (join) {
			pthread_join(thread, NULL);
			peers[i].state = 0;
		}
	}
}
