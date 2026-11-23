/*
 * 持久化层的可选 MySQL 审计队列。
 * 队列拥有事件副本，后台线程执行数据库操作。Store 写锁在队列锁之前。
 */
#include "kvs_mysql.h"
#include "kvs_config.h"
#include "kvs_allocator.h"

#if ENABLE_MYSQL

#define KVS_MYSQL_QUEUE_CAPACITY 4096
#define KVS_MYSQL_BATCH_SIZE 128

#include <mysql/mysql.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct kvs_mysql_event_s {
	char *cmd;
	char *key;
	char *value;
	int source;
} kvs_mysql_event_t;

static MYSQL kvs_mysql_conn;
static MYSQL_STMT *kvs_mysql_statement;
static pthread_t kvs_mysql_thread;
static int kvs_mysql_started;
static int kvs_mysql_stopping;
static int kvs_mysql_ready;
static int kvs_mysql_init_result;

static kvs_mysql_event_t kvs_mysql_queue[KVS_MYSQL_QUEUE_CAPACITY];
static size_t kvs_mysql_queue_head;
static size_t kvs_mysql_queue_tail;
static size_t kvs_mysql_queue_count;

static pthread_mutex_t kvs_mysql_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kvs_mysql_condition = PTHREAD_COND_INITIALIZER;

static int kvs_mysql_table_name_valid(const char *name) {
	const unsigned char *cursor;

	if (name == NULL || *name == '\0') return 0;
	for (cursor = (const unsigned char *)name; *cursor; cursor++) {
		if (!((*cursor >= 'a' && *cursor <= 'z') ||
			(*cursor >= 'A' && *cursor <= 'Z') ||
			(*cursor >= '0' && *cursor <= '9') ||
			*cursor == '_')) {
			return 0;
		}
	}
	return 1;
}

static int kvs_mysql_exec(const char *sql) {
	if (mysql_real_query(&kvs_mysql_conn, sql, (unsigned long)strlen(sql)) != 0) {
		fprintf(stderr, "mysql query error: %s\n", mysql_error(&kvs_mysql_conn));
		return -1;
	}
	return 0;
}

static int kvs_mysql_create_table(void) {
	char sql[1024];
	int length;

	if (!kvs_mysql_table_name_valid(kvs_config_get()->mysql_table)) return -1;
	length = snprintf(
		sql,
		sizeof(sql),
		"CREATE TABLE IF NOT EXISTS %s ("
		"id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
		"cmd VARCHAR(16) NOT NULL,"
		"key_name VARCHAR(255) NOT NULL,"
		"value_text TEXT NULL,"
		"source INT NOT NULL,"
		"created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
		"INDEX idx_key_name (key_name),"
		"INDEX idx_created_at (created_at)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
		kvs_config_get()->mysql_table
	);
	if (length < 0 || (size_t)length >= sizeof(sql)) return -1;
	return kvs_mysql_exec(sql);
}

static int kvs_mysql_prepare_statement(void) {
	char sql[512];
	int length;

	length = snprintf(
		sql,
		sizeof(sql),
		"INSERT INTO %s(cmd, key_name, value_text, source) "
		"VALUES(?, ?, ?, ?)",
		kvs_config_get()->mysql_table
	);
	if (length < 0 || (size_t)length >= sizeof(sql)) return -1;

	kvs_mysql_statement = mysql_stmt_init(&kvs_mysql_conn);
	if (kvs_mysql_statement == NULL ||
		mysql_stmt_prepare(
			kvs_mysql_statement,
			sql,
			(unsigned long)length
		) != 0) {
		fprintf(stderr, "mysql statement prepare: %s\n",
			kvs_mysql_statement ?
				mysql_stmt_error(kvs_mysql_statement) :
				mysql_error(&kvs_mysql_conn));
		return -1;
	}
	return 0;
}

static char *kvs_mysql_copy_string(const char *text) {
	size_t length;
	char *copy;

	if (text == NULL) return NULL;
	length = strlen(text);
	copy = kvs_malloc(length + 1);
	if (copy != NULL) memcpy(copy, text, length + 1);
	return copy;
}

static void kvs_mysql_event_destroy(kvs_mysql_event_t *event) {
	kvs_free(event->cmd);
	kvs_free(event->key);
	kvs_free(event->value);
	memset(event, 0, sizeof(*event));
}

static int kvs_mysql_event_create(
	kvs_mysql_event_t *event,
	const char *cmd,
	const char *key,
	const char *value,
	kvs_event_source_t source
) {
	memset(event, 0, sizeof(*event));
	event->cmd = kvs_mysql_copy_string(cmd);
	event->key = kvs_mysql_copy_string(key);
	event->value = kvs_mysql_copy_string(value);
	event->source = (int)source;
	if (event->cmd == NULL || event->key == NULL ||
		(value != NULL && event->value == NULL)) {
		kvs_mysql_event_destroy(event);
		return -1;
	}
	return 0;
}

static int kvs_mysql_execute_event(const kvs_mysql_event_t *event) {
	MYSQL_BIND bind[4];
	unsigned long cmd_length = (unsigned long)strlen(event->cmd);
	unsigned long key_length = (unsigned long)strlen(event->key);
	unsigned long value_length = event->value == NULL ?
		0UL : (unsigned long)strlen(event->value);

	memset(bind, 0, sizeof(bind));
	bind[0].buffer_type = MYSQL_TYPE_STRING;
	bind[0].buffer = event->cmd;
	bind[0].buffer_length = cmd_length;
	bind[0].length = &cmd_length;
	bind[1].buffer_type = MYSQL_TYPE_STRING;
	bind[1].buffer = event->key;
	bind[1].buffer_length = key_length;
	bind[1].length = &key_length;
	if (event->value == NULL) {
		bind[2].buffer_type = MYSQL_TYPE_NULL;
	} else {
		bind[2].buffer_type = MYSQL_TYPE_STRING;
		bind[2].buffer = event->value;
		bind[2].buffer_length = value_length;
		bind[2].length = &value_length;
	}
	bind[3].buffer_type = MYSQL_TYPE_LONG;
	bind[3].buffer = (void *)&event->source;

	if (mysql_stmt_bind_param(kvs_mysql_statement, bind) != 0 ||
		mysql_stmt_execute(kvs_mysql_statement) != 0) {
		fprintf(stderr, "mysql write: %s\n",
			mysql_stmt_error(kvs_mysql_statement));
		return -1;
	}
	return 0;
}

static int kvs_mysql_connect(void) {
	if (mysql_init(&kvs_mysql_conn) == NULL) {
		fprintf(stderr, "mysql_init failed\n");
		return -1;
	}
	if (mysql_real_connect(
			&kvs_mysql_conn,
			kvs_config_get()->mysql_host,
			kvs_config_get()->mysql_user,
			kvs_config_get()->mysql_password,
			kvs_config_get()->mysql_database,
			(unsigned int)kvs_config_get()->mysql_port,
			NULL,
			0
		) == NULL) {
		fprintf(stderr, "mysql_real_connect: %s\n",
			mysql_error(&kvs_mysql_conn));
		mysql_close(&kvs_mysql_conn);
		return -1;
	}
	if (mysql_set_character_set(&kvs_mysql_conn, "utf8mb4") != 0 ||
		kvs_mysql_create_table() != 0 ||
		mysql_autocommit(&kvs_mysql_conn, 0) != 0 ||
		kvs_mysql_prepare_statement() != 0) {
		fprintf(stderr, "mysql setup failed: %s\n",
			mysql_error(&kvs_mysql_conn));
		if (kvs_mysql_statement != NULL) {
			mysql_stmt_close(kvs_mysql_statement);
			kvs_mysql_statement = NULL;
		}
		mysql_close(&kvs_mysql_conn);
		return -1;
	}
	return 0;
}

static void kvs_mysql_disconnect(void) {
	if (kvs_mysql_statement != NULL) {
		mysql_stmt_close(kvs_mysql_statement);
		kvs_mysql_statement = NULL;
	}
	mysql_close(&kvs_mysql_conn);
}

static size_t kvs_mysql_take_batch(
	kvs_mysql_event_t batch[KVS_MYSQL_BATCH_SIZE]
) {
	size_t count = 0;

	pthread_mutex_lock(&kvs_mysql_mutex);
	while (kvs_mysql_queue_count == 0 && !kvs_mysql_stopping) {
		pthread_cond_wait(&kvs_mysql_condition, &kvs_mysql_mutex);
	}
	while (count < KVS_MYSQL_BATCH_SIZE && kvs_mysql_queue_count > 0) {
		batch[count++] = kvs_mysql_queue[kvs_mysql_queue_head];
		memset(
			&kvs_mysql_queue[kvs_mysql_queue_head],
			0,
			sizeof(kvs_mysql_queue[kvs_mysql_queue_head])
		);
		kvs_mysql_queue_head =
			(kvs_mysql_queue_head + 1) % KVS_MYSQL_QUEUE_CAPACITY;
		kvs_mysql_queue_count--;
	}
	pthread_mutex_unlock(&kvs_mysql_mutex);
	return count;
}

static void *kvs_mysql_worker(void *arg) {
	int connect_result;

	(void)arg;
	connect_result = kvs_mysql_connect();
	pthread_mutex_lock(&kvs_mysql_mutex);
	kvs_mysql_init_result = connect_result;
	kvs_mysql_ready = 1;
	pthread_cond_signal(&kvs_mysql_condition);
	pthread_mutex_unlock(&kvs_mysql_mutex);
	if (connect_result != 0) return NULL;

	for (;;) {
		kvs_mysql_event_t batch[KVS_MYSQL_BATCH_SIZE];
		size_t count = kvs_mysql_take_batch(batch);
		int batch_ok = 1;

		if (count == 0) break;
		for (size_t i = 0; i < count; i++) {
			if (batch_ok && kvs_mysql_execute_event(&batch[i]) != 0) {
				batch_ok = 0;
			}
		}
		if (batch_ok) {
			if (mysql_commit(&kvs_mysql_conn) != 0) {
				fprintf(stderr, "mysql commit: %s\n",
					mysql_error(&kvs_mysql_conn));
				(void)mysql_rollback(&kvs_mysql_conn);
			}
		} else {
			(void)mysql_rollback(&kvs_mysql_conn);
		}
		for (size_t i = 0; i < count; i++) {
			kvs_mysql_event_destroy(&batch[i]);
		}
	}
	kvs_mysql_disconnect();
	return NULL;
}

int kvs_mysql_start(void) {
	pthread_mutex_lock(&kvs_mysql_mutex);
	if (kvs_mysql_started) {
		pthread_mutex_unlock(&kvs_mysql_mutex);
		return 0;
	}
	kvs_mysql_queue_head = 0;
	kvs_mysql_queue_tail = 0;
	kvs_mysql_queue_count = 0;
	kvs_mysql_stopping = 0;
	kvs_mysql_ready = 0;
	kvs_mysql_init_result = -1;
	pthread_mutex_unlock(&kvs_mysql_mutex);

	if (pthread_create(&kvs_mysql_thread, NULL, kvs_mysql_worker, NULL) != 0) {
		return -1;
	}

	pthread_mutex_lock(&kvs_mysql_mutex);
	while (!kvs_mysql_ready) {
		pthread_cond_wait(&kvs_mysql_condition, &kvs_mysql_mutex);
	}
	if (kvs_mysql_init_result == 0) kvs_mysql_started = 1;
	pthread_mutex_unlock(&kvs_mysql_mutex);
	if (kvs_mysql_init_result != 0) {
		pthread_join(kvs_mysql_thread, NULL);
		return -1;
	}
	return 0;
}

void kvs_mysql_stop(void) {
	pthread_mutex_lock(&kvs_mysql_mutex);
	if (!kvs_mysql_started) {
		pthread_mutex_unlock(&kvs_mysql_mutex);
		return;
	}
	kvs_mysql_stopping = 1;
	pthread_cond_signal(&kvs_mysql_condition);
	pthread_mutex_unlock(&kvs_mysql_mutex);
	pthread_join(kvs_mysql_thread, NULL);

	pthread_mutex_lock(&kvs_mysql_mutex);
	kvs_mysql_started = 0;
	pthread_mutex_unlock(&kvs_mysql_mutex);
}

int kvs_mysql_append(
	const char *cmd,
	const char *key,
	const char *value,
	kvs_event_source_t source
) {
	kvs_mysql_event_t event;

	if (cmd == NULL || key == NULL ||
		kvs_mysql_event_create(&event, cmd, key, value, source) != 0) {
		return -1;
	}

	pthread_mutex_lock(&kvs_mysql_mutex);
	if (!kvs_mysql_started || kvs_mysql_stopping ||
		kvs_mysql_queue_count == KVS_MYSQL_QUEUE_CAPACITY) {
		pthread_mutex_unlock(&kvs_mysql_mutex);
		kvs_mysql_event_destroy(&event);
		return -1;
	}
	kvs_mysql_queue[kvs_mysql_queue_tail] = event;
	kvs_mysql_queue_tail =
		(kvs_mysql_queue_tail + 1) % KVS_MYSQL_QUEUE_CAPACITY;
	kvs_mysql_queue_count++;
	pthread_cond_signal(&kvs_mysql_condition);
	pthread_mutex_unlock(&kvs_mysql_mutex);
	return 0;
}

#endif
