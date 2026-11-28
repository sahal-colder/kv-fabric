#define _GNU_SOURCE
#include "../../src/worker/spdk_bridge.h"
#include <spdk/bdev.h>
#include <spdk/thread.h>
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int mode;
static spdk_bdev_event_cb_t event_fn;
static void *event_arg;
int __real_spdk_bdev_open_ext(const char *,bool,spdk_bdev_event_cb_t,void *,struct spdk_bdev_desc **);
int __wrap_spdk_bdev_open_ext(const char *n,bool w,spdk_bdev_event_cb_t fn,void *arg,struct spdk_bdev_desc **d) {
    event_fn=fn; event_arg=arg; return __real_spdk_bdev_open_ext(n,w,fn,arg,d);
}
int __real_spdk_bdev_read(struct spdk_bdev_desc *,struct spdk_io_channel *,void *,uint64_t,uint64_t,spdk_bdev_io_completion_cb,void *);
static spdk_bdev_io_completion_cb original_cb;
static void *original_arg;
static void failed_completion(struct spdk_bdev_io *io,bool success,void *arg) {
    (void)success; (void)arg; original_cb(io,false,original_arg);
}
int __wrap_spdk_bdev_read(struct spdk_bdev_desc *d,struct spdk_io_channel *ch,void *p,
                          uint64_t off,uint64_t n,spdk_bdev_io_completion_cb cb,void *arg) {
    assert(spdk_get_thread());
    if(mode==1) { mode=0; return -ENOMEM; }
    if(mode==2) { mode=0; return -EIO; }
    if(mode==3) { mode=0; original_cb=cb; original_arg=arg;
        return __real_spdk_bdev_read(d,ch,p,off,n,failed_completion,arg); }
    if(mode==4) { mode=0; event_fn(SPDK_BDEV_EVENT_REMOVE,spdk_bdev_desc_get_bdev(d),event_arg); return -EIO; }
    return __real_spdk_bdev_read(d,ch,p,off,n,cb,arg);
}
int __wrap_spdk_bdev_queue_io_wait(struct spdk_bdev *b,struct spdk_io_channel *ch,struct spdk_bdev_io_wait_entry *entry) {
    (void)b; (void)ch;
    /* Deterministic resource wakeup; never invoke an owning-thread callback from a producer. */
    return spdk_thread_send_msg(spdk_get_thread(),entry->cb_fn,entry->cb_arg);
}
int main(void) {
    char path[]="/tmp/kvf-spdk-faults.XXXXXX";int fd=mkstemp(path);assert(fd>=0);
    const char json[]="{\"subsystems\":[{\"subsystem\":\"iobuf\",\"config\":[{\"method\":\"iobuf_set_options\",\"params\":{\"small_pool_count\":1024,\"large_pool_count\":64}}]},{\"subsystem\":\"bdev\",\"config\":[{\"method\":\"bdev_set_options\",\"params\":{\"bdev_io_pool_size\":256,\"bdev_io_cache_size\":32}},{\"method\":\"bdev_malloc_create\",\"params\":{\"name\":\"FaultMalloc\",\"num_blocks\":1024,\"block_size\":4096}}]}]}";
    assert(write(fd,json,sizeof(json)-1)==sizeof(json)-1);close(fd);
    cpu_set_t cpus;assert(!sched_getaffinity(0,sizeof(cpus),&cpus));unsigned cpu=0;
    while(cpu<64&&!CPU_ISSET(cpu,&cpus)) ++cpu;
    assert(cpu<64);char mask[32];snprintf(mask,sizeof(mask),"0x%llx",1ull<<cpu);
    mi_spdk_config cfg={.json_config=path,.bdev_name="FaultMalloc",.reactor_mask=mask,
        .capacity_bytes=4*1024*1024,.no_huge=1,.no_pci=1};
    mi_spdk_device *d=NULL;
    assert(mi_spdk_open_ex(cfg,(mi_spdk_options){.queue_depth=4,.buffer_count=1},&d)==MI_EINVAL);
    assert(mi_spdk_open(cfg,&d)==MI_OK);unlink(path);
    char a[16384],b[16384];memset(a,0x5a,sizeof(a));
    assert(mi_spdk_transfer(d,1,(mi_io_range){.data=a,.data_bytes=sizeof(a)})==MI_OK);
    mode=1;assert(mi_spdk_transfer(d,0,(mi_io_range){.data=b,.data_bytes=sizeof(b)})==MI_OK);
    assert(!memcmp(a,b,sizeof(a)));
    mode=2;assert(mi_spdk_transfer(d,0,(mi_io_range){.data=b,.data_bytes=sizeof(b)})==MI_EIO);
    mode=3;assert(mi_spdk_transfer(d,0,(mi_io_range){.data=b,.data_bytes=sizeof(b)})==MI_EIO);
    /* Failed requests return pool slots; a subsequent complete read still works. */
    assert(mi_spdk_transfer(d,0,(mi_io_range){.data=b,.data_bytes=sizeof(b)})==MI_OK);
    mode=4;assert(mi_spdk_transfer(d,0,(mi_io_range){.data=b,.data_bytes=sizeof(b)})==MI_EIO);
    assert(mi_spdk_transfer(d,0,(mi_io_range){.data=b,.data_bytes=sizeof(b)})==MI_EIO);
    mi_spdk_stats stats;mi_spdk_get_stats(d,&stats);
    assert(stats.pool_waits==1 && stats.submitted==stats.completed && !stats.inflight);
    mi_spdk_close(d);puts("SPDK ENOMEM/wakeup, submit/completion errors, removal event, reclaim: PASS");return 0;
}
