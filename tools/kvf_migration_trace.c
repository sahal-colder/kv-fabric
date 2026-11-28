/* Instrumentation only: link the existing migration benchmark object and
 * production library with --wrap. No replacement I/O implementation. */
#define _POSIX_C_SOURCE 200809L
#include "../src/worker/spdk_bridge.h"
#include <nvtx3/nvToolsExt.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
mi_status __real_mi_spdk_transfer(mi_spdk_device *, int, mi_io_range);
mi_status __real_mi_uring_transfer(mi_uring *, int, mi_io_range);
mi_status __wrap_mi_spdk_transfer(mi_spdk_device *d, int write, mi_io_range range) {
    nvtxRangePushA(write ? "kvfabric.profile.ssd.write_wait" : "kvfabric.profile.ssd.read_wait");
    mi_status st=__real_mi_spdk_transfer(d,write,range);
    nvtxRangePop();
    return st;
}
mi_status __wrap_mi_uring_transfer(mi_uring *r, int write, mi_io_range range) {
    nvtxRangePushA(write ? "kvfabric.profile.ssd.write_wait" : "kvfabric.profile.ssd.read_wait");
    mi_status st=__real_mi_uring_transfer(r,write,range);
    nvtxRangePop();
    return st;
}

mi_status __real_mi_ssd_raw_uring_backend_create(const char *,uint64_t,size_t,size_t,mi_ssd_verify,unsigned,int,unsigned,mi_storage_backend **);
mi_status __wrap_mi_ssd_raw_uring_backend_create(const char *path,uint64_t offset,size_t capacity,
        size_t bytes,mi_ssd_verify verify,unsigned depth,int device,unsigned cpu,mi_storage_backend **out) {
    errno=0;
    mi_status st=__real_mi_ssd_raw_uring_backend_create(path,offset,capacity,bytes,verify,depth,device,cpu,out);
    if(st!=MI_OK) fprintf(stderr,"profile raw open status=%d errno=%d (%s) path=%s\n",st,errno,strerror(errno),path);
    return st;
}
