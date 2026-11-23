/*
 * Remote Connector background work. The ticket owns copied tokens and the
 * result sequence until wait transfers it. Queue lock covers FIFO state only.
 * It is released before lookup, socket I/O, and publication. Tickets must be
 * consumed or destroyed before Worker shutdown.
 */
#include "internal.h"
#include <stdlib.h>
#include <string.h>

enum { PREFETCH_DEPTH = 8 };

struct mi_remote_prefetch {
    pthread_mutex_t lock;      /* Protects the completion and ownership handoff. */
    pthread_cond_t changed;    /* Signals completion after the result is ready. */
    atomic_bool cancelled;    /* Observed at page and layer boundaries. */
    int done;                  /* Set exactly once by the worker. */
    int claimed;               /* Only one waiter may take the result. */
    kvf_status status;         /* Failure to start resolution or obtain a sequence. */
    mi_kv_sequence *sequence; /* Owned until claimed or destroyed. */
    mi_remote_result result;  /* Includes the safe prefix and any remote failure. */
    size_t count;              /* Number of copied tokens. */
    int32_t tokens[];          /* Immutable after admission, valid until completion. */
};

struct mi_prefetch_queue {
    pthread_mutex_t lock;                             /* Protects indices, current ticket and stop. */
    pthread_cond_t work;                               /* Wakes the service thread for entries or stop. */
    pthread_t thread;                                  /* Joined before releasing Worker/Trie. */
    mi_remote_prefetch *jobs[PREFETCH_DEPTH];           /* The FIFO holds only queued tickets. */
    mi_remote_prefetch *running;                       /* Current job, or NULL. */
    unsigned head, count;                              /* Occupied range wraps modulo depth. */
    int stopping;                                      /* No new entries after shutdown begins. */
};

static void *serve(void *arg) {
    mi_remote_worker *w = arg;
    mi_prefetch_queue *q = w->prefetch;
    pthread_mutex_lock(&q->lock);
    for (;;) {
        while (!q->count && !q->stopping)
            pthread_cond_wait(&q->work, &q->lock);
        if (!q->count)
            break;
        mi_remote_prefetch *j = q->jobs[q->head];
        q->head = (q->head + 1) % PREFETCH_DEPTH;
        --q->count;
        q->running = j;
        pthread_mutex_unlock(&q->lock);

        j->status = mi_remote_resolve_impl(w, j->tokens, j->count, &j->sequence, &j->result, &j->cancelled);

        pthread_mutex_lock(&q->lock);
        q->running = NULL;
        pthread_mutex_unlock(&q->lock);
        /* After signalling, the waiter may free j immediately. */
        pthread_mutex_lock(&j->lock);
        j->done = 1;
        pthread_cond_broadcast(&j->changed);
        pthread_mutex_unlock(&j->lock);
        pthread_mutex_lock(&q->lock);
    }
    pthread_mutex_unlock(&q->lock);
    return NULL;
}

kvf_status mi_prefetch_start(mi_remote_worker *w) {
    mi_prefetch_queue *q = calloc(1, sizeof(*q));
    if (!q)
        return KVF_LIMIT;
    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        free(q);
        return KVF_INTERNAL;
    }
    if (pthread_cond_init(&q->work, NULL) != 0) {
        pthread_mutex_destroy(&q->lock);
        free(q);
        return KVF_INTERNAL;
    }
    w->prefetch = q;
    if (pthread_create(&q->thread, NULL, serve, w) == 0)
        return KVF_OK;
    w->prefetch = NULL;
    pthread_cond_destroy(&q->work);
    pthread_mutex_destroy(&q->lock);
    free(q);
    return KVF_INTERNAL;
}

void mi_prefetch_stop(mi_remote_worker *w) {
    mi_prefetch_queue *q = w->prefetch;
    if (!q)
        return;
    pthread_mutex_lock(&q->lock);
    q->stopping = 1;
    mi_remote_prefetch_cancel(q->running);
    for (unsigned i = 0; i < q->count; ++i)
        mi_remote_prefetch_cancel(q->jobs[(q->head + i) % PREFETCH_DEPTH]);
    pthread_cond_signal(&q->work);
    pthread_mutex_unlock(&q->lock);
    pthread_join(q->thread, NULL);
    pthread_cond_destroy(&q->work);
    pthread_mutex_destroy(&q->lock);
    free(q);
    w->prefetch = NULL;
}

kvf_status mi_remote_prefetch_async(mi_remote_worker *w, const int32_t *tokens, size_t count,
                                     mi_remote_prefetch **out) {
    if (!w || !tokens || !count || !out || count / w->identity.tokens_per_page > w->cache->max_pages ||
        count > (SIZE_MAX - sizeof(mi_remote_prefetch)) / sizeof(*tokens))
        return KVF_INVALID;
    mi_remote_prefetch *j = calloc(1, sizeof(*j) + count * sizeof(*tokens));
    if (!j)
        return KVF_LIMIT;
    if (pthread_mutex_init(&j->lock, NULL) != 0) {
        free(j);
        return KVF_INTERNAL;
    }
    if (pthread_cond_init(&j->changed, NULL) != 0) {
        pthread_mutex_destroy(&j->lock);
        free(j);
        return KVF_INTERNAL;
    }
    atomic_init(&j->cancelled, 0);
    j->count = count;
    memcpy(j->tokens, tokens, count * sizeof(*tokens));
    mi_prefetch_queue *q = w->prefetch;
    pthread_mutex_lock(&q->lock);
    kvf_status st = KVF_LIMIT;
    if (!q->stopping && q->count < PREFETCH_DEPTH) {
        q->jobs[(q->head + q->count++) % PREFETCH_DEPTH] = j;
        pthread_cond_signal(&q->work);
        st = KVF_OK;
    }
    pthread_mutex_unlock(&q->lock);
    if (st == KVF_OK) {
        *out = j;
    } else {
        pthread_cond_destroy(&j->changed);
        pthread_mutex_destroy(&j->lock);
        free(j);
    }
    return st;
}

kvf_status mi_remote_prefetch_wait(mi_remote_prefetch *j, mi_kv_sequence **out, mi_remote_result *result) {
    if (!j || !out || !result)
        return KVF_INVALID;
    pthread_mutex_lock(&j->lock);
    while (!j->done)
        pthread_cond_wait(&j->changed, &j->lock);
    kvf_status st = KVF_CONFLICT;
    if (!j->claimed) {
        *out = j->sequence;
        *result = j->result;
        j->sequence = NULL;
        j->claimed = 1;
        st = j->status;
    }
    pthread_mutex_unlock(&j->lock);
    return st;
}

void mi_remote_prefetch_cancel(mi_remote_prefetch *j) {
    if (j)
        atomic_store_explicit(&j->cancelled, 1, memory_order_relaxed);
}

void mi_remote_prefetch_destroy(mi_remote_prefetch *j) {
    if (!j)
        return;
    mi_remote_prefetch_cancel(j);
    mi_kv_sequence *sequence = NULL;
    mi_remote_result result;
    (void)mi_remote_prefetch_wait(j, &sequence, &result);
    mi_kv_sequence_destroy(sequence);
    pthread_cond_destroy(&j->changed);
    pthread_mutex_destroy(&j->lock);
    free(j);
}
