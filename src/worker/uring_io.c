/* One ring owner batches SQEs and drains CQEs. Caller ranges stay pinned until wait. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "io_transfer.h"
#include "miniinfer/device_transfer.h"
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef MI_URING_TEST
extern struct io_uring_sqe *mi_test_get_sqe(struct io_uring *);
extern int mi_test_wait_cqe(struct io_uring *, struct io_uring_cqe **);
extern int mi_test_peek_cqe(struct io_uring *, struct io_uring_cqe **);
#define io_uring_get_sqe mi_test_get_sqe
#define io_uring_wait_cqe mi_test_wait_cqe
#define io_uring_peek_cqe mi_test_peek_cqe
#endif
enum { FREE, QUEUED, ACTIVE, COMPLETE, MAX_QD=128 };
struct mi_uring_request {
    mi_uring *ring;
    pthread_cond_t changed;
    atomic_bool cancelled;
    mi_io_range range;
    mi_io_metrics metrics;
    unsigned char *buffer;
    size_t position, chunk;
    int state, write, legacy, pending;
    unsigned interrupts, index;
    mi_status status;
};
struct mi_uring {
    struct io_uring ring;
    mi_uring_options options;
    mi_uring_stats stats;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t worker;
    mi_uring_request *requests, **queue;
    unsigned head, queued, owned, submitters, prepared;
    int fd, failed, started, closing, quiesced;
    void *allocation, *buffer;
    size_t capacity;
    int device, legacy_busy;
};
static mi_status error_status(int err) {
    if (err == -ENOMEM) return MI_ENOMEM;
    if (err == -EINVAL || err == -ENOSYS || err == -EOPNOTSUPP || err == -EPERM)
        return MI_EUNSUPPORTED;
    return MI_EIO;
}
mi_status mi_uring_create_ex(int fd, mi_uring_options o, mi_uring **out) {
    if (!out) return MI_EINVAL;
    *out=NULL;
    if (fd<0 || !o.queue_depth || o.queue_depth>MAX_QD) return MI_EINVAL;
    if (!o.submit_batch) o.submit_batch=o.queue_depth;
    if (!o.buffer_bytes) o.buffer_bytes=1024*1024;
    if (o.worker_cpu_plus_one>CPU_SETSIZE || o.submit_batch>MAX_QD || o.buffer_bytes%4096 || o.buffer_bytes>8*1024*1024 ||
        (o.iopoll && !(fcntl(fd,F_GETFL)&O_DIRECT))) return MI_EINVAL;
    mi_uring *r=calloc(1,sizeof(*r));
    if (!r) return MI_ENOMEM;
    r->options=o; r->fd=fd; r->device=-1;
    r->requests=calloc(o.queue_depth,sizeof(*r->requests));
    r->queue=calloc(o.queue_depth,sizeof(*r->queue));
    mi_status st=MI_ENOMEM;
    unsigned initialized=0;
    if (!r->requests || !r->queue) goto free_all;
    st=MI_EINTERNAL;
    if (pthread_mutex_init(&r->mutex,NULL)) goto free_all;
    if (pthread_cond_init(&r->changed,NULL)) goto free_mutex;
    struct io_uring_params params={0};
    if (o.iopoll) params.flags|=IORING_SETUP_IOPOLL;
    if (o.sqpoll) {
        params.flags|=IORING_SETUP_SQPOLL; params.sq_thread_idle=1000;
        if(o.worker_cpu_plus_one) { params.flags|=IORING_SETUP_SQ_AFF; params.sq_thread_cpu=o.worker_cpu_plus_one-1; }
    }
    int err=io_uring_queue_init_params(o.queue_depth,&r->ring,&params);
    if (err<0) { st=error_status(err); goto free_cond; }
    struct iovec buffers[MAX_QD];
    for (; initialized<o.queue_depth; ++initialized) {
        mi_uring_request *q=&r->requests[initialized];
        q->ring=r; q->index=initialized; atomic_init(&q->cancelled,false);
        if (pthread_cond_init(&q->changed,NULL)) { st=MI_EINTERNAL; goto free_ring; }
        if (posix_memalign((void **)&q->buffer,4096,o.buffer_bytes)) {
            pthread_cond_destroy(&q->changed); st=MI_ENOMEM; goto free_ring;
        }
        buffers[initialized]=(struct iovec){q->buffer,o.buffer_bytes};
    }
    if (o.fixed_buffers) {
        err=io_uring_register_buffers(&r->ring,buffers,o.queue_depth);
        if (err<0) { st=error_status(err); goto free_ring; }
        r->stats.fixed_buffers=1;
    }
    if (o.fixed_file) {
        err=io_uring_register_files(&r->ring,&fd,1);
        if (err<0) { st=error_status(err); goto free_ring; }
        r->stats.fixed_file=1;
    }
    r->stats.iopoll=!!(params.flags&IORING_SETUP_IOPOLL);
    r->stats.sqpoll=!!(params.flags&IORING_SETUP_SQPOLL);
    *out=r;
    return MI_OK;
free_ring:
    io_uring_queue_exit(&r->ring);
    for (unsigned i=0;i<initialized;++i) {
        free(r->requests[i].buffer); pthread_cond_destroy(&r->requests[i].changed);
    }
free_cond:
    pthread_cond_destroy(&r->changed);
free_mutex:
    pthread_mutex_destroy(&r->mutex);
free_all:
    free(r->requests); free(r->queue); free(r);
    return st;
}
mi_status mi_uring_create(int fd, unsigned depth, mi_uring **out) {
    return mi_uring_create_ex(fd,(mi_uring_options){.queue_depth=depth},out);
}
int mi_uring_register_buffer(mi_uring *r, int device, size_t bytes) {
    if (!r || !bytes || bytes>SIZE_MAX-4095 || r->started || r->buffer || r->stats.fixed_buffers)
        return -EINVAL;
    mi_status st=mi_device_host_alloc(device,bytes+4095,&r->allocation);
    if (st!=MI_OK) return st==MI_ENOMEM ? -ENOMEM : -EIO;
    r->buffer=(char *)r->allocation+((-(uintptr_t)r->allocation)&4095);
    r->capacity=bytes; r->device=device;
    struct iovec buf={r->buffer,bytes};
    int error=io_uring_register_buffers(&r->ring,&buf,1);
    if (!error) r->stats.fixed_buffers=1;
    return error;
}
void *mi_uring_buffer(mi_uring *r) {
    if (!r) return NULL;
    pthread_mutex_lock(&r->mutex);
    void *p=r->stats.fixed_buffers && !r->failed ? r->buffer : NULL;
    pthread_mutex_unlock(&r->mutex);
    return p;
}
static void copy_buffer(mi_uring_request *q, size_t bytes, int to_buffer) {
    uint64_t begin=mi_io_now_ns();
    struct iovec v[2]; size_t n;
    unsigned count=mi_io_segments(v,q->range,q->position,bytes,&n);
    unsigned char *p=q->buffer;
    for (unsigned i=0;i<count;++i) {
        if (to_buffer) memcpy(p,v[i].iov_base,v[i].iov_len);
        else memcpy(v[i].iov_base,p,v[i].iov_len);
        p+=v[i].iov_len;
    }
    q->metrics.copy_ns+=mi_io_now_ns()-begin;
}
static void finish(mi_uring_request *q, mi_status st) {
    mi_uring *r=q->ring;
    pthread_mutex_lock(&r->mutex);
    q->metrics.completed_ns=mi_io_now_ns();
    r->stats.copy_ns+=q->metrics.copy_ns;
    q->status=st; q->state=COMPLETE;
    pthread_cond_signal(&q->changed);
    pthread_mutex_unlock(&r->mutex);
}
/* This only prepares an SQE. All buffers belong to the ring, even on hard errors. */
static int prepare(mi_uring_request *q) {
    mi_uring *r=q->ring;
    size_t remaining=q->range.header_bytes+q->range.data_bytes-q->position;
    if (atomic_load_explicit(&q->cancelled,memory_order_acquire)) {
        finish(q,MI_EBUSY); return 0;
    }
    if (!remaining) { finish(q,MI_OK); return 0; }
    struct io_uring_sqe *sqe=io_uring_get_sqe(&r->ring);
    if (!sqe) {
        if (io_uring_sq_space_left(&r->ring)) return -EIO;
        /* SQPOLL can publish CQEs before publishing its new SQ head. A full
         * submission ring is backpressure, not a failed operation. */
        pthread_mutex_lock(&r->mutex);
        q->state=QUEUED;
        r->queue[(r->head+r->queued++)%r->options.queue_depth]=q;
        pthread_mutex_unlock(&r->mutex);
        sched_yield();
        return 0;
    }
    q->chunk=remaining<r->options.buffer_bytes ? remaining : r->options.buffer_bytes;
    void *p=q->buffer;
    if (q->legacy) p=(char *)r->buffer+q->position;
    else if (q->write) copy_buffer(q,q->chunk,1);
    int fd=r->stats.fixed_file ? 0 : r->fd;
    int fixed=q->legacy || (r->options.fixed_buffers && !r->buffer);
    unsigned index=q->legacy ? 0 : q->index;
    uint64_t offset=q->range.offset+q->position;
    if (fixed && q->write) io_uring_prep_write_fixed(sqe,fd,p,(unsigned)q->chunk,offset,index);
    else if (fixed) io_uring_prep_read_fixed(sqe,fd,p,(unsigned)q->chunk,offset,index);
    else if (q->write) io_uring_prep_write(sqe,fd,p,(unsigned)q->chunk,offset);
    else io_uring_prep_read(sqe,fd,p,(unsigned)q->chunk,offset);
    if (r->stats.fixed_file) sqe->flags|=IOSQE_FIXED_FILE;
    io_uring_sqe_set_data(sqe,q);
    q->pending=1;
    ++r->prepared;
    if (!q->metrics.started_ns) q->metrics.started_ns=mi_io_now_ns();
    return 1;
}
static int quiesce(mi_uring *r) {
    pthread_mutex_lock(&r->mutex);
    unsigned inflight=r->stats.inflight;
    pthread_mutex_unlock(&r->mutex);
    if (r->quiesced || !inflight) return 1;
    struct io_uring_sync_cancel_reg cancel={
        .flags=IORING_ASYNC_CANCEL_ANY|IORING_ASYNC_CANCEL_ALL,.timeout={.tv_sec=1}};
    int err=io_uring_register_sync_cancel(&r->ring,&cancel);
    if (err>=0 || err==-ENOENT) {
        r->quiesced=1;
        pthread_mutex_lock(&r->mutex); r->stats.inflight=0; pthread_mutex_unlock(&r->mutex);
        return 1;
    }
    /* One CQE proves only one operation ended. Drain all before freeing a pool. */
    struct io_uring_cqe *cqe;
    unsigned completed=0;
    while (!io_uring_peek_cqe(&r->ring,&cqe) && cqe) {
        io_uring_cqe_seen(&r->ring,cqe); ++completed;
    }
    pthread_mutex_lock(&r->mutex);
    r->stats.inflight=inflight-completed;
    pthread_mutex_unlock(&r->mutex);
    if (completed>=inflight) { r->quiesced=1; return 1; }
    return 0;
}
static void fail_ring(mi_uring *r) {
    /* Ambiguous submit may have accepted any prepared SQE. Count conservatively
     * before cancellation. No request ever borrows caller-owned DMA memory. */
    unsigned possible=0;
    for (unsigned i=0;i<r->options.queue_depth;++i) possible+=(unsigned)r->requests[i].pending;
    pthread_mutex_lock(&r->mutex); r->stats.inflight=possible; pthread_mutex_unlock(&r->mutex);
    (void)quiesce(r);
    pthread_mutex_lock(&r->mutex);
    r->failed=1; r->queued=0;
    for (unsigned i=0;i<r->options.queue_depth;++i) {
        mi_uring_request *q=&r->requests[i];
        if (q->state==ACTIVE || q->state==QUEUED) {
            q->state=COMPLETE; q->status=MI_EIO; pthread_cond_signal(&q->changed);
        }
    }
    pthread_cond_broadcast(&r->changed);
    pthread_mutex_unlock(&r->mutex);
}
static int submit_sqes(mi_uring *r) {
    unsigned interrupted=0;
    if (r->stats.sqpoll) {
        if (!r->prepared) return 0;
        int n;
        do { n=io_uring_submit(&r->ring); } while(n==-EINTR && ++interrupted<16);
        if(n<0) return n;
        /* SQPOLL owns every newly published SQE. sq_ready may still include
         * already published entries until its kernel thread advances SQ head;
         * repeatedly counting that value would invent submissions and hang CQ. */
        pthread_mutex_lock(&r->mutex);
        r->stats.submitted+=r->prepared;
        r->stats.inflight+=r->prepared;
        if(r->stats.inflight>r->stats.max_inflight) r->stats.max_inflight=r->stats.inflight;
        pthread_mutex_unlock(&r->mutex);
        r->prepared=0;
        return 0;
    }
    while (io_uring_sq_ready(&r->ring)) {
        int n=io_uring_submit(&r->ring);
        if (n==-EINTR && ++interrupted<16) continue;
        if (n<=0) return -EIO;
        pthread_mutex_lock(&r->mutex);
        r->stats.submitted+=(unsigned)n;
        r->stats.inflight+=(unsigned)n;
        if (r->stats.inflight>r->stats.max_inflight) r->stats.max_inflight=r->stats.inflight;
        pthread_mutex_unlock(&r->mutex);
    }
    r->prepared=0;
    return 0;
}
static void *engine(void *arg) {
    mi_uring *r=arg;
    for (;;) {
        pthread_mutex_lock(&r->mutex);
        while (!r->queued && !r->stats.inflight && !(r->closing && !r->owned && !r->submitters))
            pthread_cond_wait(&r->changed,&r->mutex);
        int shutdown=r->closing && !r->owned && !r->submitters;
        unsigned n=r->queued;
        if (n>r->options.submit_batch) n=r->options.submit_batch;
        mi_uring_request *batch[MAX_QD];
        for (unsigned i=0;i<n;++i) {
            batch[i]=r->queue[r->head]; r->head=(r->head+1)%r->options.queue_depth;
            --r->queued; batch[i]->state=ACTIVE;
        }
        pthread_mutex_unlock(&r->mutex);
        if (shutdown) return NULL;
        for (unsigned i=0;i<n;++i) if (prepare(batch[i])<0) goto failure;
        if (submit_sqes(r)<0) goto failure;
        if (!r->stats.inflight) continue;
        /* Producers may have filled more slots while this batch was prepared.
         * Fill available queue depth before blocking for a completion. */
        pthread_mutex_lock(&r->mutex);
        int more=r->queued!=0;
        pthread_mutex_unlock(&r->mutex);
        if (more) continue;
        struct io_uring_cqe *cqe=NULL;
        unsigned interrupted=0;
        int err;
        do { err=io_uring_wait_cqe(&r->ring,&cqe); } while (err==-EINTR && ++interrupted<16);
        if (err<0 || !cqe) goto failure;
        unsigned harvested=0;
        do {
            mi_uring_request *q=io_uring_cqe_get_data(cqe);
            int result=cqe->res;
            q->pending=0;
            io_uring_cqe_seen(&r->ring,cqe);
            pthread_mutex_lock(&r->mutex);
            --r->stats.inflight; ++r->stats.completed;
            pthread_mutex_unlock(&r->mutex);
            if (result==-EINTR && ++q->interrupts<16) {
                /* Retry this chunk, preserving independent request progress. */
            } else if (result<=0 || (size_t)result>q->chunk ||
                       ((q->legacy || r->stats.iopoll) && (size_t)result<q->chunk && result%4096)) {
                finish(q,MI_EIO); q=NULL;
            } else {
                if (!q->write && !q->legacy) copy_buffer(q,(size_t)result,0);
                q->position+=(size_t)result; q->interrupts=0;
            }
            if (q && prepare(q)<0) goto failure;
            if (++harvested==r->options.submit_batch) break;
            cqe=NULL;
        } while (!io_uring_peek_cqe(&r->ring,&cqe) && cqe);
        if (submit_sqes(r)<0) goto failure;
    }
failure:
    fail_ring(r);
    return NULL;
}
static mi_status submit(mi_uring *r,int write,mi_io_range range,mi_uring_request **out,int wait,int legacy) {
    if (!r || !out) return MI_EINVAL;
    *out=NULL;
    if (range.header_bytes>SIZE_MAX-range.data_bytes || (range.header_bytes && !range.header) ||
        (range.data_bytes && !range.data)) return MI_EINVAL;
    size_t total=range.header_bytes+range.data_bytes;
    if (range.offset>INT64_MAX || total>INT64_MAX-range.offset ||
        (legacy && (range.header_bytes || range.offset%4096 || total%4096 ||
                    total>r->capacity || !r->stats.fixed_buffers))) return MI_EINVAL;
    pthread_mutex_lock(&r->mutex);
    ++r->submitters;
    while (!r->failed && !r->closing &&
           (r->owned==r->options.queue_depth || (legacy && r->legacy_busy))) {
        ++r->stats.queue_waits;
        if (!wait) { --r->submitters; pthread_mutex_unlock(&r->mutex); return MI_EBUSY; }
        pthread_cond_wait(&r->changed,&r->mutex);
    }
    mi_status st=MI_OK;
    if (r->failed || r->closing) { st=MI_EIO; goto done; }
    if (!r->started) {
        pthread_attr_t attr;
        if (pthread_attr_init(&attr)) { st=MI_EINTERNAL; goto done; }
        int err=0;
        if (r->options.worker_cpu_plus_one) {
            cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(r->options.worker_cpu_plus_one-1,&cpus);
            err=pthread_attr_setaffinity_np(&attr,sizeof(cpus),&cpus);
        }
        if (!err) err=pthread_create(&r->worker,&attr,engine,r);
        pthread_attr_destroy(&attr);
        if (err) { st=MI_EINTERNAL; goto done; }
        r->started=1;
    }
    mi_uring_request *q=r->requests;
    while (q->state!=FREE) ++q;
    q->metrics=(mi_io_metrics){.queued_ns=mi_io_now_ns()};
    q->range=range; q->write=!!write; q->position=0; q->legacy=legacy; q->interrupts=0;
    if (legacy) {
        r->legacy_busy=1;
        if (write && range.data!=r->buffer) memcpy(r->buffer,range.data,total);
    }
    atomic_store_explicit(&q->cancelled,false,memory_order_relaxed);
    q->state=QUEUED;
    r->queue[(r->head+r->queued++)%r->options.queue_depth]=q; ++r->owned;
    *out=q;
    pthread_cond_signal(&r->changed);
done:
    --r->submitters;
    if (r->closing) pthread_cond_broadcast(&r->changed);
    pthread_mutex_unlock(&r->mutex);
    return st;
}
mi_status mi_uring_submit(mi_uring *r,int write,mi_io_range range,mi_uring_request **out) {
    return submit(r,write,range,out,0,0);
}
mi_status mi_uring_wait_metrics(mi_uring_request *q,mi_io_metrics *metrics) {
    if (!q) return MI_EINVAL;
    mi_uring *r=q->ring;
    pthread_mutex_lock(&r->mutex);
    while (q->state!=COMPLETE) pthread_cond_wait(&q->changed,&r->mutex);
    mi_status st=q->status;
    if (metrics) *metrics=q->metrics;
    if (q->legacy) {
        if (st==MI_OK && !q->write && q->range.data!=r->buffer)
            memcpy(q->range.data,r->buffer,q->range.data_bytes);
        r->legacy_busy=0;
    }
    q->state=FREE; --r->owned;
    pthread_cond_broadcast(&r->changed);
    pthread_mutex_unlock(&r->mutex);
    return st;
}
mi_status mi_uring_wait(mi_uring_request *q) { return mi_uring_wait_metrics(q,NULL); }
void mi_uring_cancel(mi_uring_request *q) {
    if (q) atomic_store_explicit(&q->cancelled,true,memory_order_release);
}
mi_status mi_uring_transfer(mi_uring *r,int write,mi_io_range range) {
    mi_uring_request *q; mi_status st=submit(r,write,range,&q,1,0);
    return st==MI_OK ? mi_uring_wait(q) : st;
}
mi_status mi_uring_transfer_fixed(mi_uring *r,int write,mi_io_range range) {
    mi_uring_request *q; mi_status st=submit(r,write,range,&q,1,1);
    return st==MI_OK ? mi_uring_wait(q) : st;
}
mi_status mi_uring_transfer_batch(mi_uring *r,int write,const mi_io_range *ranges,size_t count,mi_status *results) {
    if (!r || !ranges || !results || !count) return MI_EINVAL;
    mi_status first=MI_OK;
    for (size_t base=0;base<count;) {
        mi_uring_request *pending[MAX_QD]={0};
        size_t n=count-base;
        if (n>r->options.queue_depth) n=r->options.queue_depth;
        size_t accepted=0;
        for (;accepted<n;++accepted) {
            mi_status st=submit(r,write,ranges[base+accepted],&pending[accepted],!accepted,0);
            if (st==MI_EBUSY) break;
            results[base+accepted]=st;
        }
        n=accepted;
        for (size_t i=0;i<n;++i) {
            if (pending[i]) results[base+i]=mi_uring_wait(pending[i]);
            if (first==MI_OK) first=results[base+i];
        }
        base+=n;
    }
    return first;
}
void mi_uring_get_stats(mi_uring *r,mi_uring_stats *out) {
    if (!r || !out) return;
    pthread_mutex_lock(&r->mutex); *out=r->stats; pthread_mutex_unlock(&r->mutex);
}
void mi_uring_destroy(mi_uring *r) {
    if (!r) return;
    pthread_mutex_lock(&r->mutex); r->closing=1; pthread_cond_broadcast(&r->changed); pthread_mutex_unlock(&r->mutex);
    if (r->started) { pthread_join(r->worker,NULL); r->started=0; }
    /* A failed engine exits after publishing errors. Its request owners may
     * still be waking; retain their conditions/ranges until every wait consumes. */
    pthread_mutex_lock(&r->mutex);
    while (r->owned || r->submitters) pthread_cond_wait(&r->changed,&r->mutex);
    pthread_mutex_unlock(&r->mutex);
    if (!quiesce(r)) {
        fprintf(stderr,"MiniInfer: quarantining io_uring with unconfirmed I/O completion\n");
        return;
    }
    if (r->stats.fixed_file) (void)io_uring_unregister_files(&r->ring);
    if (r->stats.fixed_buffers) (void)io_uring_unregister_buffers(&r->ring);
    io_uring_queue_exit(&r->ring);
    if (r->allocation) (void)mi_device_host_free(r->device,r->allocation);
    for (unsigned i=0;i<r->options.queue_depth;++i) {
        free(r->requests[i].buffer); pthread_cond_destroy(&r->requests[i].changed);
    }
    pthread_cond_destroy(&r->changed); pthread_mutex_destroy(&r->mutex);
    free(r->queue); free(r->requests); free(r);
}
