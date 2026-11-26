#define _POSIX_C_SOURCE 200809L
#include "miniinfer/miniinfer.h"
#include "miniinfer/device_transfer.h"
#include "../../src/worker/direct_io.h"
#include "../../src/worker/tiered_internal.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

enum { BYTES=4096 };
static int fail_write,fail_release,fail_wait,fail_fence;
static mi_stream failed_stream;
static void *failed_host;
static int concurrent_snapshots;
static pthread_barrier_t snapshot_barrier;
mi_status __real_mi_device_copy_to_host(int,void *,const void *,size_t,mi_stream);
mi_status __wrap_mi_device_copy_to_host(int d,void *h,const void *p,size_t n,mi_stream stream) {
    if(fail_fence)
        failed_host=h;
    if(concurrent_snapshots) {
        int rc=pthread_barrier_wait(&snapshot_barrier);
        assert(rc==0 || rc==PTHREAD_BARRIER_SERIAL_THREAD);
    }
    return __real_mi_device_copy_to_host(d,h,p,n,stream);
}
static unsigned disk_releases;
static mi_storage_backend *disk;
mi_status __real_mi_backend_write_record(mi_storage_backend *,mi_storage_handle,const mi_block_meta *,const void *);
mi_status __wrap_mi_backend_write_record(mi_storage_backend *b,mi_storage_handle h,const mi_block_meta *m,const void *p) {
    if(fail_write){fail_write=0;return MI_EIO;}return __real_mi_backend_write_record(b,h,m,p);
}
mi_status __real_mi_backend_release(mi_storage_backend *,mi_storage_handle);
mi_status __wrap_mi_backend_release(mi_storage_backend *b,mi_storage_handle h) {
    if(b==disk){disk_releases++;if(fail_release){fail_release=0;return MI_EIO;}}
    return __real_mi_backend_release(b,h);
}
mi_status __real_mi_device_stream_wait(int,mi_stream);
mi_status __wrap_mi_device_stream_wait(int d,mi_stream s) {
    if(fail_wait){fail_wait--;failed_stream=s;return MI_EIO;}return __real_mi_device_stream_wait(d,s);
}
mi_status __real_mi_device_event_record(int,mi_event,mi_stream);
mi_status __wrap_mi_device_event_record(int d,mi_event e,mi_stream s) {
    if(fail_fence)
        return MI_EIO;
    return __real_mi_device_event_record(d,e,s);
}
typedef struct {mi_tiered_storage *storage;mi_block_id id;unsigned lane;} concurrent;
static void *stress(void *arg) {
    concurrent *c=arg;
    for(unsigned i=0;i<10000;i++) {
        mi_status st;
        if(c->lane==0) {
            unsigned char data[BYTES];mi_block_meta meta;
            st=mi_storage_read_snapshot(c->storage,c->id,0,data,BYTES,NULL,&meta);
            if(st==MI_OK)
                for(unsigned j=0;j<BYTES;j++)assert(data[j]==data[0]);
            else assert(st==MI_EBUSY);
        } else if(c->lane==1) {
            mi_lease l;st=mi_storage_acquire(c->storage,c->id,MI_ACCESS_WRITE,NULL,&l);
            if(st==MI_OK){memset(l.data,(int)(i%251),BYTES);assert(mi_lease_finish(&l)==MI_OK);}
            else assert(st==MI_EBUSY || st==MI_ESTATE);
        } else {
            st=mi_storage_migrate(c->storage,c->id,(i+c->lane)%2?MI_LOCATION_CPU:MI_LOCATION_SSD);
            assert(st==MI_OK || st==MI_EBUSY);
        }
    }
    return NULL;
}
static void migration(void) {
    mi_mempool *pool;mi_storage_backend *cpu;mi_tiered_storage *s;
    assert(mi_mempool_create(MI_MEMORY_CPU,-1,(mi_mempool_config){0},&pool)==MI_OK);
    assert(mi_memory_backend_create(MI_LOCATION_CPU,BYTES,pool,2,&cpu)==MI_OK);
    assert(mi_ssd_backend_create(NULL,BYTES,(BYTES+64)*2,MI_SSD_VERIFY_READ_BACK,&disk)==MI_OK);
    mi_storage_config c;mi_storage_config_default(&c);c.block_bytes=BYTES;c.cpu_bytes=BYTES*2;c.ssd_bytes=(BYTES+64)*2;c.retain_ssd_copy=1;
    assert(mi_tiered_storage_create(c,(mi_storage_backends){.cpu=cpu,.ssd=disk},&s)==MI_OK);
    mi_block_id id;assert(mi_storage_create_block(s,BYTES,MI_LOCATION_CPU,&id)==MI_OK);mi_lease l;
    assert(mi_storage_acquire(s,id,MI_ACCESS_WRITE,NULL,&l)==MI_OK);memset(l.data,0x59,BYTES);assert(mi_lease_finish(&l)==MI_OK);
    assert(mi_storage_migrate(s,id,MI_LOCATION_SSD)==MI_OK);assert(mi_storage_migrate(s,id,MI_LOCATION_CPU)==MI_OK);
    unsigned before=disk_releases;fail_write=1;
    assert(mi_storage_migrate(s,id,MI_LOCATION_SSD)==MI_EIO);
    /* Only the failed destination is released; the preexisting replica remains. */
    assert(disk_releases==before+1);mi_block_meta m;unsigned char data[BYTES];
    assert(mi_storage_read_snapshot(s,id,1,data,BYTES,NULL,&m)==MI_OK && m.location==MI_LOCATION_CPU);
    for(unsigned i=0;i<BYTES;i++)assert(data[i]==0x59);
    fail_write=1;fail_release=1;assert(mi_storage_migrate(s,id,MI_LOCATION_SSD)==MI_EROLLBACK_CLEANUP);
    assert(mi_storage_retry_cleanup(s,id)==MI_OK);
    fail_release=1;
    assert(mi_storage_migrate(s,id,MI_LOCATION_SSD)==MI_ECLEANUP);
    assert(mi_storage_migrate(s,id,MI_LOCATION_CPU)==MI_EBUSY);
    assert(mi_storage_retry_cleanup(s,id)==MI_OK);
    assert(mi_storage_migrate(s,id,MI_LOCATION_CPU)==MI_OK);
    assert(mi_storage_migrate(s,id,MI_LOCATION_SSD)==MI_OK);
    assert(mi_storage_read_snapshot(s,id,1,data,BYTES,NULL,&m)==MI_OK);for(unsigned i=0;i<BYTES;i++)assert(data[i]==0x59);
    assert(mi_storage_migrate(s,id,MI_LOCATION_CPU)==MI_OK);
    pthread_t threads[4];concurrent tasks[4];
    for(unsigned i=0;i<4;i++){tasks[i]=(concurrent){s,id,i};assert(!pthread_create(&threads[i],NULL,stress,&tasks[i]));}
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    assert(mi_storage_metadata(s,id,&m)==MI_OK && !m.lease_count);
    assert(mi_storage_migrate(s,id,MI_LOCATION_CPU)==MI_OK);
    mi_paged_kv *kv;assert(mi_paged_kv_create(s,1,2,BYTES,"lease-fault",&kv)==MI_OK);mi_kv_sequence *q;
    assert(mi_kv_sequence_create(kv,&q)==MI_OK);assert(mi_kv_sequence_append_page(q,MI_LOCATION_CPU)==MI_OK);
    mi_page_lease pl;assert(mi_paged_kv_acquire(q,0,0,MI_ACCESS_WRITE,NULL,&pl)==MI_OK);
    /* CPU event stubs give a deterministic completed fence after a failed wait. */
    pl.lease.stream=(mi_stream)(uintptr_t)1;fail_wait=1;
    assert(mi_page_lease_finish(&pl)==MI_EIO);assert(!pl.kv && !pl.lease.active);
    assert(mi_page_lease_finish(&pl)==MI_EINVAL);
    mi_kv_sequence_destroy(q);mi_paged_kv_destroy(kv);
    assert(mi_storage_erase(s,id)==MI_OK);mi_tiered_storage_destroy(s);mi_backend_destroy(disk);disk=NULL;mi_backend_destroy(cpu);mi_mempool_destroy(pool);
}
/* Losing the caller's stack lease must not lose the physical DMA owner. */
static void lease_retirement(void) {
    mi_mempool *pool;mi_storage_backend *cpu;mi_tiered_storage *s;
    assert(mi_mempool_create(MI_MEMORY_CPU,-1,(mi_mempool_config){0},&pool)==MI_OK);
    assert(mi_memory_backend_create(MI_LOCATION_CPU,BYTES,pool,1,&cpu)==MI_OK);
    mi_storage_config c;mi_storage_config_default(&c);c.block_bytes=BYTES;c.cpu_bytes=BYTES;
    assert(mi_tiered_storage_create(c,(mi_storage_backends){.cpu=cpu},&s)==MI_OK);
    mi_paged_kv *kv;mi_kv_sequence *q;mi_page_lease pl;
    assert(mi_paged_kv_create(s,1,2,BYTES,"retire",&kv)==MI_OK);
    assert(mi_kv_sequence_create(kv,&q)==MI_OK);
    assert(mi_kv_sequence_append_page(q,MI_LOCATION_CPU)==MI_OK);
    assert(mi_paged_kv_acquire(q,0,0,MI_ACCESS_WRITE,NULL,&pl)==MI_OK);
    mi_block_id id=pl.lease.id;void *data=pl.lease.data;
    pl.lease.stream=(mi_stream)(uintptr_t)1;fail_wait=1;fail_fence=1;
    assert(mi_page_lease_abort(&pl)==MI_EIO);
    assert(!pl.kv && !pl.lease.active);
    assert(mi_page_lease_abort(&pl)==MI_EINVAL);
    mi_block_meta meta;assert(mi_storage_metadata(s,id,&meta)==MI_OK);
    assert(meta.lease_count==0 && meta.state==MI_BLOCK_ERROR);
    mi_kv_sequence_destroy(q);mi_paged_kv_destroy(kv);
    assert(mi_storage_erase(s,id)==MI_EIO);
    mi_lease l;assert(mi_storage_acquire(s,id,MI_ACCESS_WRITE,NULL,&l)==MI_EIO);
    assert(mi_storage_migrate(s,id,MI_LOCATION_CPU)==MI_EIO);
    assert(mi_storage_retry_cleanup(s,id)==MI_EIO);
    assert(!mi_storage_has_room(s,MI_LOCATION_CPU));
    mi_block_id other;assert(mi_storage_create_block(s,BYTES,MI_LOCATION_CPU,&other)==MI_EIO);
    /* All three destructors run, but the retained Storage still owns the slab. */
    mi_tiered_storage_destroy(s);mi_backend_destroy(cpu);mi_mempool_destroy(pool);
    memset(data,0x5b,BYTES);
    fail_fence=0;
}
#ifdef MINIINFER_WITH_CUDA
static void *snapshot_reader(void *arg) {
    concurrent *c=arg;mi_stream stream;mi_event event;mi_block_meta meta;
    assert(mi_device_stream_create(0,&stream)==MI_OK);assert(mi_device_event_create(0,&event)==MI_OK);
    unsigned char data[BYTES];
    assert(mi_storage_read_snapshot_event(c->storage,c->id,1,data,BYTES,stream,event,&meta)==MI_OK);
    for(unsigned i=0;i<BYTES;i++)assert(data[i]==0x62);
    assert(mi_device_event_destroy(0,event)==MI_OK);assert(mi_device_stream_destroy(0,stream)==MI_OK);
    return NULL;
}
static void cuda_parallel_snapshots(void) {
    mi_mempool *pool;mi_storage_backend *gpu;mi_tiered_storage *s;mi_stream stream;
    assert(mi_mempool_create(MI_MEMORY_DEVICE,0,(mi_mempool_config){0},&pool)==MI_OK);
    assert(mi_memory_backend_create(MI_LOCATION_GPU,BYTES,pool,1,&gpu)==MI_OK);
    mi_storage_config c;mi_storage_config_default(&c);c.block_bytes=BYTES;c.cpu_bytes=0;c.gpu_bytes=BYTES;
    assert(mi_tiered_storage_create(c,(mi_storage_backends){.gpu=gpu},&s)==MI_OK);
    mi_block_id id;mi_lease l;unsigned char data[BYTES];memset(data,0x62,BYTES);
    assert(mi_device_stream_create(0,&stream)==MI_OK);
    assert(mi_storage_create_block(s,BYTES,MI_LOCATION_GPU,&id)==MI_OK);
    assert(mi_storage_acquire(s,id,MI_ACCESS_WRITE,stream,&l)==MI_OK);
    assert(mi_device_copy_from_host(0,l.data,data,BYTES,stream)==MI_OK);
    assert(mi_lease_finish(&l)==MI_OK);
    assert(!pthread_barrier_init(&snapshot_barrier,NULL,64));concurrent_snapshots=1;
    pthread_t threads[64];concurrent task={s,id,0};
    for(unsigned i=0;i<64;i++)assert(!pthread_create(&threads[i],NULL,snapshot_reader,&task));
    for(unsigned i=0;i<64;i++)assert(!pthread_join(threads[i],NULL));
    concurrent_snapshots=0;assert(!pthread_barrier_destroy(&snapshot_barrier));
    assert(mi_storage_erase(s,id)==MI_OK);mi_tiered_storage_destroy(s);
    mi_backend_destroy(gpu);mi_mempool_destroy(pool);assert(mi_device_stream_destroy(0,stream)==MI_OK);
}
static void cuda_retirement(int snapshot, int direct) {
    mi_mempool *pool,*host_pool=NULL;mi_storage_backend *gpu,*ssd=NULL,*cpu=NULL;mi_tiered_storage *s;
    assert(mi_mempool_create(MI_MEMORY_DEVICE,0,(mi_mempool_config){0},&pool)==MI_OK);
    assert(mi_memory_backend_create(MI_LOCATION_GPU,BYTES,pool,1,&gpu)==MI_OK);
    if(direct==1 || direct==2) assert(mi_ssd_direct_backend_create(NULL,BYTES,8192,MI_SSD_VERIFY_READ_BACK,1,0,&ssd)==MI_OK);
    if(direct==3) {
        assert(mi_mempool_create(MI_MEMORY_CPU,-1,(mi_mempool_config){0},&host_pool)==MI_OK);
        assert(mi_memory_backend_create(MI_LOCATION_CPU,BYTES,host_pool,1,&cpu)==MI_OK);
    }
    mi_storage_config c;mi_storage_config_default(&c);c.block_bytes=BYTES;c.cpu_bytes=direct==3?BYTES:0;c.gpu_bytes=BYTES;c.ssd_bytes=ssd?8192:0;
    assert(mi_tiered_storage_create(c,(mi_storage_backends){.gpu=gpu,.ssd=ssd,.cpu=cpu},&s)==MI_OK);
    mi_block_id id;mi_lease l;mi_stream stream;mi_event event;
    assert(mi_device_stream_create(0,&stream)==MI_OK);assert(mi_device_event_create(0,&event)==MI_OK);
    assert(mi_storage_create_block(s,BYTES,MI_LOCATION_GPU,&id)==MI_OK);
    assert(mi_storage_acquire(s,id,MI_ACCESS_WRITE,stream,&l)==MI_OK);
    unsigned char host[BYTES];memset(host,0x62,BYTES);void *device=l.data;
    assert(mi_device_copy_from_host(0,l.data,host,BYTES,stream)==MI_OK);
    if(snapshot || direct) assert(mi_lease_finish(&l)==MI_OK);
    if(direct==2) assert(mi_storage_migrate(s,id,MI_LOCATION_SSD)==MI_OK);
    fail_wait=1;fail_fence=1;
    if(snapshot) {
        mi_block_meta m;memset(host,0xa7,BYTES);
        mi_status st=snapshot==1
            ? mi_storage_read_snapshot_event(s,id,1,host,BYTES,stream,event,&m)
            : mi_storage_read_to_host(s,id,host,BYTES,NULL);
        assert(st==MI_EIO);
        for(unsigned i=0;i<BYTES;i++)assert(host[i]==0xa7);
    } else if(direct) {
        mi_location target=direct==1?MI_LOCATION_SSD:direct==2?MI_LOCATION_GPU:MI_LOCATION_CPU;
        assert(mi_storage_migrate(s,id,target)==MI_EIO);
    } else {
        assert(mi_lease_finish(&l)==MI_EIO && !l.active);
    }
    fail_fence=0;fail_wait=0;
    assert(mi_storage_erase(s,id)==MI_EIO);
    mi_block_meta m;assert(mi_storage_metadata(s,id,&m)==MI_OK && m.lease_count==0);
    mi_tiered_storage_destroy(s);mi_backend_destroy(gpu);mi_backend_destroy(ssd);mi_mempool_destroy(pool);
    mi_backend_destroy(cpu);mi_mempool_destroy(host_pool);
    /* A real delayed operation may finish after every public destructor. */
    assert(__real_mi_device_stream_wait(0,failed_stream)==MI_OK);
    assert(mi_device_copy_to_host(0,host,device,BYTES,NULL)==MI_OK);
    for(unsigned i=0;i<BYTES;i++)assert(host[i]==0x62);
    if(direct==3)
        for(unsigned i=0;i<BYTES;i++)assert(((unsigned char *)failed_host)[i]==0x62);
    assert(mi_device_event_destroy(0,event)==MI_OK);assert(mi_device_stream_destroy(0,stream)==MI_OK);
}
#endif
static void direct_boundaries(void) {
    char path[]="/tmp/kvf-audit-direct.XXXXXX";int fd=mkstemp(path);assert(fd>=0);assert(!ftruncate(fd,8192));
    mi_direct_io *io=NULL;
    assert(mi_direct_create(fd,-1,8192,0,&io)==MI_EINVAL);
    assert(mi_direct_create(fd,-1,8192,65,&io)==MI_EINVAL);
    unsigned char data[BYTES],out[BYTES];memset(data,0x38,BYTES);
    mi_block_meta m={.id=1,.version=1,.size=BYTES};
    const unsigned depths[]={1,64};
    for(unsigned j=0;j<2;j++) {
        assert(mi_direct_create(fd,-1,8192,depths[j],&io)==MI_OK);
        assert(mi_direct_record(io,1,0,&m,data,-1,MI_SSD_VERIFY_READ_BACK)==MI_OK);
        /* A separate buffered fd can deliberately corrupt an O_DIRECT record. */
        int corrupt=open(path,O_RDWR);assert(corrupt>=0);unsigned char byte=0x39;
        assert(pwrite(corrupt,&byte,1,4096+17)==1);
        assert(mi_direct_record(io,0,0,&m,out,-1,MI_SSD_VERIFY_NONE)==MI_OK && out[17]==byte);
        assert(mi_direct_record(io,0,0,&m,out,-1,MI_SSD_VERIFY_READ_BACK)==MI_ECRC);
        assert(pwrite(corrupt,&byte,1,8)==1);
        assert(mi_direct_record(io,0,0,&m,out,-1,MI_SSD_VERIFY_NONE)==MI_ECRC);
        assert(mi_direct_record(io,0,1,&m,out,-1,MI_SSD_VERIFY_NONE)==MI_EINVAL);
        close(corrupt);mi_direct_destroy(io);
    }
    close(fd);unlink(path);
}
int main(int argc,char **argv) {
#ifdef MINIINFER_WITH_CUDA
    if(argc>1 && !strcmp(argv[1],"cuda")) {
        cuda_parallel_snapshots();
        cuda_retirement(0,0);cuda_retirement(1,0);cuda_retirement(2,0);
        cuda_retirement(0,1);cuda_retirement(0,2);cuda_retirement(0,3);
        puts("CUDA lease/snapshot/Direct retirement survives all parent destructors: PASS");return 0;
    }
#else
    (void)argc;(void)argv;
#endif
    migration();direct_boundaries();lease_retirement();
    puts("migration rollback + replica preservation; lease retirement; direct boundaries/CRC: PASS");return 0;
}
