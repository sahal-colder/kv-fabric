#define _GNU_SOURCE
#include "../../src/worker/io_transfer.h"
#ifdef TEST_SPDK
#include "../../src/worker/spdk_bridge.h"
#include <sched.h>
#endif
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
enum { COUNT=128, BYTES=16384, TOTAL=COUNT*BYTES };
#ifdef TEST_SPDK
typedef mi_spdk_device device;
typedef mi_spdk_request request;
#define SUBMIT mi_spdk_submit
#define WAIT mi_spdk_wait
#define CANCEL mi_spdk_cancel
#define BATCH mi_spdk_transfer_batch
#define CLOSE mi_spdk_close
#else
typedef mi_uring device;
typedef mi_uring_request request;
#define SUBMIT mi_uring_submit
#define WAIT mi_uring_wait
#define CANCEL mi_uring_cancel
#define BATCH mi_uring_transfer_batch
#define CLOSE mi_uring_destroy
#endif
typedef struct task { device *d; mi_io_range *ranges; } task;
static void *reader(void *arg) {
    task *t=arg;
    mi_status results[COUNT];
    assert(BATCH(t->d,0,t->ranges,COUNT,results)==MI_OK);
    for (unsigned i=0;i<COUNT;++i) assert(results[i]==MI_OK);
    return NULL;
}
static void *closer(void *arg) { CLOSE(arg); return NULL; }
int main(int argc,char **argv) {
    assert(argc==2); unsigned qd=(unsigned)strtoul(argv[1],NULL,10);
    device *d=NULL;
#ifdef TEST_SPDK
    char path[]="/tmp/kvf-async-spdk.XXXXXX"; int fd=mkstemp(path); assert(fd>=0);
    const char json[]="{\"subsystems\":[{\"subsystem\":\"iobuf\",\"config\":[{\"method\":\"iobuf_set_options\",\"params\":{\"small_pool_count\":1024,\"large_pool_count\":64}}]},{\"subsystem\":\"bdev\",\"config\":[{\"method\":\"bdev_set_options\",\"params\":{\"bdev_io_pool_size\":256,\"bdev_io_cache_size\":32}},{\"method\":\"bdev_malloc_create\",\"params\":{\"name\":\"AsyncMalloc\",\"num_blocks\":2048,\"block_size\":4096}}]}]}";
    assert(write(fd,json,sizeof(json)-1)==sizeof(json)-1); close(fd);
    cpu_set_t cpus; assert(!sched_getaffinity(0,sizeof(cpus),&cpus)); unsigned cpu=0;
    while(cpu<64&&!CPU_ISSET(cpu,&cpus)) ++cpu;
    assert(cpu<64);
    char mask[32]; snprintf(mask,sizeof(mask),"0x%llx",1ull<<cpu);
    mi_spdk_config cfg={.json_config=path,.bdev_name="AsyncMalloc",.reactor_mask=mask,
        .capacity_bytes=8*1024*1024,.no_huge=1,.no_pci=1};
    assert(mi_spdk_open_ex(cfg,(mi_spdk_options){.queue_depth=qd,.buffer_count=COUNT,
                .max_transfer_bytes=4096,.submit_batch=qd},&d)==MI_OK); unlink(path);
#else
    char path[]="/tmp/kvf-async-uring.XXXXXX"; int fd=mkstemp(path); assert(fd>=0); unlink(path);
    assert(!ftruncate(fd,TOTAL));
    mi_status st=mi_uring_create_ex(fd,(mi_uring_options){.queue_depth=qd,
        .fixed_file=1,.fixed_buffers=1,.buffer_bytes=4096,.sqpoll=getenv("KVF_TEST_SQPOLL")!=NULL},&d);
    if(st==MI_EUNSUPPORTED) { close(fd); return 77; } assert(st==MI_OK);
#endif
    unsigned char *source=malloc(TOTAL),*actual=calloc(1,TOTAL),*other=calloc(1,TOTAL);
    assert(source&&actual&&other);
    mi_io_range ranges[COUNT],reads[COUNT],reads2[COUNT]; mi_status results[COUNT];
    for(unsigned i=0;i<TOTAL;++i) source[i]=(unsigned char)(i*61u+(i/BYTES)*17u);
    for(unsigned i=0;i<COUNT;++i) {
        ranges[i]=(mi_io_range){.data=source+i*BYTES,.data_bytes=BYTES,.offset=i*BYTES};
        reads[i]=(mi_io_range){.data=actual+i*BYTES,.data_bytes=BYTES,.offset=i*BYTES};
        reads2[i]=(mi_io_range){.data=other+i*BYTES,.data_bytes=BYTES,.offset=i*BYTES};
    }
    assert(BATCH(d,1,ranges,COUNT,results)==MI_OK);
    /* Two simultaneous windows must not deadlock while sharing a finite pool. */
    pthread_t a,b; task ta={d,reads},tb={d,reads2};
    assert(!pthread_create(&a,NULL,reader,&ta)); assert(!pthread_create(&b,NULL,reader,&tb));
    pthread_join(a,NULL); pthread_join(b,NULL);
    assert(!memcmp(source,actual,TOTAL)&&!memcmp(source,other,TOTAL));
    /* Completed but unconsumed requests retain their slots. Exhaustion is deterministic. */
#ifdef TEST_SPDK
    unsigned pool=COUNT;
#else
    unsigned pool=qd;
#endif
    request *pending[COUNT],*extra=NULL;
    for(unsigned i=0;i<pool;++i) assert(SUBMIT(d,0,reads[i],&pending[i])==MI_OK);
    assert(SUBMIT(d,0,reads[0],&extra)==MI_EBUSY && !extra);
    CANCEL(pending[pool-1]);
    for(unsigned i=0;i<pool;++i) {
        mi_status st=WAIT(pending[i]); assert(st==MI_OK || (i==pool-1 && st==MI_EBUSY));
    }
#ifdef TEST_SPDK
    mi_spdk_stats stats; mi_spdk_get_stats(d,&stats);
    printf("submitted=%llu completed=%llu max_inflight=%u qd=%u queue_waits=%llu pool_waits=%llu\n",
        (unsigned long long)stats.submitted,(unsigned long long)stats.completed,stats.max_inflight,qd,
        (unsigned long long)stats.queue_waits,(unsigned long long)stats.pool_waits);
#else
    mi_uring_stats stats; mi_uring_get_stats(d,&stats);
    assert(stats.fixed_buffers && stats.fixed_file && !stats.iopoll && stats.sqpoll==(getenv("KVF_TEST_SQPOLL")!=NULL));
    printf("submitted=%llu completed=%llu max_inflight=%u qd=%u queue_waits=%llu\n",
        (unsigned long long)stats.submitted,(unsigned long long)stats.completed,stats.max_inflight,qd,
        (unsigned long long)stats.queue_waits);
#endif
    assert(stats.submitted==stats.completed && stats.max_inflight<=qd);
    if(qd>1) assert(stats.max_inflight>1);
    assert(SUBMIT(d,0,reads[0],&pending[0])==MI_OK);
    /* Closing cannot free a completed request before its owner consumes it. */
    assert(!pthread_create(&a,NULL,closer,d)); assert(WAIT(pending[0])==MI_OK);
    pthread_join(a,NULL);
#ifndef TEST_SPDK
    close(fd);
#endif
    free(source); free(actual); free(other); puts("async pool/batch/concurrent windows/cancel/close: PASS");
    return 0;
}
