#define _GNU_SOURCE
#include "miniinfer/miniinfer.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
enum { COUNT=256, BYTES=4096, CONCURRENCY=4 };
static pthread_barrier_t barrier;
static atomic_uint entered, active, peak;
mi_status __real_mi_backend_write_record(mi_storage_backend *,mi_storage_handle,const mi_block_meta *,const void *);
mi_status __wrap_mi_backend_write_record(mi_storage_backend *b,mi_storage_handle h,const mi_block_meta *m,const void *p) {
    unsigned n=atomic_fetch_add(&active,1)+1,old=atomic_load(&peak);
    while(n>old&&!atomic_compare_exchange_weak(&peak,&old,n)) {}
    if(atomic_fetch_add(&entered,1)<CONCURRENCY) pthread_barrier_wait(&barrier);
    mi_status st=__real_mi_backend_write_record(b,h,m,p);
    atomic_fetch_sub(&active,1); return st;
}
int main(int argc,char **argv) {
    mi_mempool *pool; mi_storage_backend *cpu,*ssd; mi_tiered_storage *s;
    assert(mi_mempool_create(MI_MEMORY_CPU,-1,(mi_mempool_config){0},&pool)==MI_OK);
    assert(mi_memory_backend_create(MI_LOCATION_CPU,BYTES,pool,COUNT,&cpu)==MI_OK);
    size_t disk_stride=BYTES+64;
#ifdef TEST_SPDK
    disk_stride=BYTES+4096;
    char path[]="/tmp/kvf-migrate-spdk.XXXXXX";int fd=mkstemp(path);assert(fd>=0);
    const char json[]="{\"subsystems\":[{\"subsystem\":\"iobuf\",\"config\":[{\"method\":\"iobuf_set_options\",\"params\":{\"small_pool_count\":1024,\"large_pool_count\":64}}]},{\"subsystem\":\"bdev\",\"config\":[{\"method\":\"bdev_set_options\",\"params\":{\"bdev_io_pool_size\":256,\"bdev_io_cache_size\":32}},{\"method\":\"bdev_malloc_create\",\"params\":{\"name\":\"MigrateMalloc\",\"num_blocks\":1024,\"block_size\":4096}}]}]}";
    assert(write(fd,json,sizeof(json)-1)==sizeof(json)-1);close(fd);
    cpu_set_t cpus;assert(!sched_getaffinity(0,sizeof(cpus),&cpus));unsigned reactor_cpu=0;
    while(reactor_cpu<64&&!CPU_ISSET(reactor_cpu,&cpus)) ++reactor_cpu;
    assert(reactor_cpu<64);char mask[32];snprintf(mask,sizeof(mask),"0x%llx",1ull<<reactor_cpu);
    mi_spdk_config sc={.json_config=path,.bdev_name="MigrateMalloc",.reactor_mask=mask,
        .capacity_bytes=4*1024*1024,.no_huge=1,.no_pci=1};
    mi_ssd_verify verify=argc>1&&!strcmp(argv[1],"strict")?MI_SSD_VERIFY_READ_BACK:MI_SSD_VERIFY_ON_READ;
    assert(mi_ssd_spdk_backend_create_ex(sc,(mi_spdk_options){.queue_depth=4},BYTES,verify,&ssd)==MI_OK);
    unlink(path);
#elif defined(TEST_DIRECT)
    (void)argc;(void)argv; disk_stride=BYTES+4096;
    mi_status opened=mi_ssd_direct_backend_create(NULL,BYTES,disk_stride*COUNT,MI_SSD_VERIFY_READ_BACK,1,-1,&ssd);
    if(opened==MI_EUNSUPPORTED) { mi_backend_destroy(cpu);mi_mempool_destroy(pool);return 77; }
    assert(opened==MI_OK);
#else
    (void)argc;(void)argv;
    assert(mi_ssd_backend_create(NULL,BYTES,disk_stride*COUNT,MI_SSD_VERIFY_READ_BACK,&ssd)==MI_OK);
#endif
    mi_storage_config cfg={.block_bytes=BYTES,.cpu_bytes=BYTES*COUNT,.ssd_bytes=disk_stride*COUNT,
        .max_async_migrations=CONCURRENCY};
    assert(mi_tiered_storage_create(cfg,(mi_storage_backends){.cpu=cpu,.ssd=ssd},&s)==MI_OK);
    mi_block_id ids[COUNT]; mi_status results[COUNT];
    for(unsigned i=0;i<COUNT;++i) {
        mi_lease lease; assert(mi_storage_create_block(s,BYTES,MI_LOCATION_CPU,&ids[i])==MI_OK);
        assert(mi_storage_acquire(s,ids[i],MI_ACCESS_WRITE,NULL,&lease)==MI_OK);
        memset(lease.data,(unsigned char)i,BYTES); assert(mi_lease_finish(&lease)==MI_OK);
    }
    assert(!pthread_barrier_init(&barrier,NULL,CONCURRENCY));
    assert(mi_storage_migrate_batch(s,ids,COUNT,MI_LOCATION_SSD,results)==MI_OK);
    assert(atomic_load(&peak)==CONCURRENCY);
    mi_migration_stats stats; mi_storage_migration_stats(s,&stats);
    assert(stats.max_running==CONCURRENCY && stats.queue_capacity==2*CONCURRENCY && !stats.running);
    for(unsigned i=0;i<COUNT;++i) {
        unsigned char data[BYTES]; mi_block_meta m;
        assert(results[i]==MI_OK);
        assert(mi_storage_read_snapshot(s,ids[i],1,data,BYTES,NULL,&m)==MI_OK);
        for(unsigned j=0;j<BYTES;++j) assert(data[j]==(unsigned char)i);
    }
    assert(mi_storage_migrate_batch(s,ids,COUNT,MI_LOCATION_CPU,results)==MI_OK);
    /* Hold all four active moves. Queue capacity is separate from concurrency. */
    pthread_barrier_destroy(&barrier);
    assert(!pthread_barrier_init(&barrier,NULL,CONCURRENCY+1));
    atomic_store(&entered,0);
    mi_migration_ticket *tickets[12];
    for(unsigned i=0;i<4;++i) assert(mi_storage_migrate_async(s,ids[i],MI_LOCATION_SSD,&tickets[i])==MI_OK);
    while(atomic_load(&entered)<4) sched_yield();
    for(unsigned i=4;i<12;++i) assert(mi_storage_migrate_async(s,ids[i],MI_LOCATION_SSD,&tickets[i])==MI_OK);
    mi_migration_ticket *extra=NULL;
    assert(mi_storage_migrate_async(s,ids[12],MI_LOCATION_SSD,&extra)==MI_EBUSY && !extra);
    mi_migration_cancel(tickets[0]); /* active copy completes, then rolls back */
    mi_migration_cancel(tickets[4]); /* queued task never moves */
    pthread_barrier_wait(&barrier);
    for(unsigned i=0;i<12;++i) {
        assert(mi_migration_wait(tickets[i])==((i==0||i==4)?MI_EBUSY:MI_OK));
        mi_migration_ticket_destroy(tickets[i]);
    }
    mi_block_meta cancelled;
    assert(mi_storage_metadata(s,ids[0],&cancelled)==MI_OK && cancelled.location==MI_LOCATION_CPU && cancelled.version==1);
    assert(mi_storage_metadata(s,ids[4],&cancelled)==MI_OK && cancelled.location==MI_LOCATION_CPU && cancelled.version==1);
    assert(mi_storage_migrate_batch(s,ids,COUNT,MI_LOCATION_CPU,results)==MI_OK);
    mi_lease lease; assert(mi_storage_acquire(s,ids[0],MI_ACCESS_READ,NULL,&lease)==MI_OK);
    assert(mi_storage_migrate(s,ids[0],MI_LOCATION_SSD)==MI_EBUSY);
    assert(mi_lease_finish(&lease)==MI_OK);
    mi_tiered_storage_destroy(s); mi_backend_destroy(ssd); mi_backend_destroy(cpu); mi_mempool_destroy(pool);
    pthread_barrier_destroy(&barrier);
    puts("256-page parallel batch, actual running=4, CRC/version/lease: PASS"); return 0;
}
