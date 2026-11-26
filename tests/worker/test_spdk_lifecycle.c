#define _GNU_SOURCE
#include "../../src/worker/spdk_bridge.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <spdk/event.h>
#include <spdk/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static pthread_t caller;
static int reject_messages;
int __real_spdk_thread_send_msg(const struct spdk_thread *, spdk_msg_fn, void *);
int __wrap_spdk_thread_send_msg(const struct spdk_thread *thread, spdk_msg_fn fn, void *arg) {
    if (reject_messages && pthread_equal(caller,pthread_self()))
        return -EIO;
    return __real_spdk_thread_send_msg(thread,fn,arg);
}
void __real_spdk_app_stop(int);
void __wrap_spdk_app_stop(int rc) {assert(!pthread_equal(caller,pthread_self()));assert(spdk_get_thread());__real_spdk_app_stop(rc);}
int main(int argc,char **argv) {
    caller=pthread_self();assert(argc==2);int missing=!strcmp(argv[1],"missing");
    char path[]="/tmp/kvf-spdk-audit.XXXXXX";int fd=mkstemp(path);assert(fd>=0);
    const char json[]="{\"subsystems\":[{\"subsystem\":\"iobuf\",\"config\":[{\"method\":\"iobuf_set_options\",\"params\":{\"small_pool_count\":1024,\"large_pool_count\":64}}]},{\"subsystem\":\"bdev\",\"config\":[{\"method\":\"bdev_set_options\",\"params\":{\"bdev_io_pool_size\":1024,\"bdev_io_cache_size\":64}},{\"method\":\"bdev_malloc_create\",\"params\":{\"name\":\"AuditMalloc\",\"num_blocks\":1024,\"block_size\":4096}}]}]}";
    assert(write(fd,json,sizeof(json)-1)==sizeof(json)-1);close(fd);
    cpu_set_t cpus;assert(!sched_getaffinity(0,sizeof(cpus),&cpus));unsigned cpu=0;while(cpu<64&&!CPU_ISSET(cpu,&cpus))cpu++;assert(cpu<64);
    char mask[32];snprintf(mask,sizeof(mask),"0x%llx",1ull<<cpu);
    mi_spdk_device *d=NULL;mi_status st=mi_spdk_open((mi_spdk_config){.json_config=path,
        .bdev_name=missing?"Absent":"AuditMalloc",.reactor_mask=mask,.capacity_bytes=4096*1024,.no_huge=1,.no_pci=1},&d);
    unlink(path);
    if(missing) {assert(st!=MI_OK && !d);puts("SPDK initialization failure returned safely: PASS");return 0;}
    assert(st==MI_OK && d);char a[4096],b[4096];memset(a,0x3a,sizeof(a));
    assert(mi_spdk_transfer(d,1,(mi_io_range){.data=a,.data_bytes=sizeof(a)})==MI_OK);
    assert(mi_spdk_transfer(d,0,(mi_io_range){.data=b,.data_bytes=sizeof(b)})==MI_OK);assert(!memcmp(a,b,sizeof(a)));
    reject_messages=1;
    assert(mi_spdk_transfer(d,0,(mi_io_range){.data=b,.data_bytes=sizeof(b)})==MI_EIO);
    mi_spdk_close(d);puts("SPDK malloc bdev I/O + owning-thread close: PASS");return 0;
}
