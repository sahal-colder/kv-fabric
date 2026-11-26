#define _GNU_SOURCE
#include "../../src/worker/io_transfer.h"
#include "miniinfer/storage_backend.h"
#include <assert.h>
#include <errno.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int mode, submit_calls, wait_calls;
struct io_uring_sqe *mi_test_get_sqe(struct io_uring *r) {
    if(mode==1)
        return NULL;
    return io_uring_get_sqe(r);
}
int mi_test_wait_cqe(struct io_uring *r,struct io_uring_cqe **cqe) {
    ++wait_calls;
    if(mode==4 || mode==10)return -EIO;
    if(mode==5 && wait_calls==1)return -EINTR;
    int result=io_uring_wait_cqe(r,cqe);
    if(!result && mode==6)(*cqe)->res=-EIO;
    if(!result && mode==7)(*cqe)->res=0;
    if(!result && mode==9)(*cqe)->res=-EINTR;
    return result;
}
int mi_test_peek_cqe(struct io_uring *r,struct io_uring_cqe **cqe) {
    return mode==10 ? -EAGAIN : io_uring_peek_cqe(r,cqe);
}
int __real_io_uring_register_sync_cancel(struct io_uring *, struct io_uring_sync_cancel_reg *);
int __wrap_io_uring_register_sync_cancel(struct io_uring *r,struct io_uring_sync_cancel_reg *cancel) {
    return mode==10 ? -EIO : __real_io_uring_register_sync_cancel(r,cancel);
}
int __real_io_uring_submit(struct io_uring *);
int __wrap_io_uring_submit(struct io_uring *r) {
    ++submit_calls;
    if(mode==2)return -EIO;
    if(mode==3 && submit_calls==1)return -EINTR;
    /* Accepted by kernel, error returned to the caller: ambiguous ownership. */
    if(mode==8){(void)__real_io_uring_submit(r);return -EIO;}
    return __real_io_uring_submit(r);
}
int main(void) {
    char path[]="/tmp/kvf-uring-fault.XXXXXX";int fd=mkstemp(path);assert(fd>=0);unlink(path);assert(!ftruncate(fd,4096));
    mi_uring *r;mi_status st=mi_uring_create(fd,2,&r);if(st==MI_EUNSUPPORTED){close(fd);return 77;}assert(st==MI_OK);mi_uring_destroy(r);
    for(mode=0;mode<=10;mode++) {
        submit_calls=wait_calls=0;assert(mi_uring_create(fd,2,&r)==MI_OK);
        char *p=malloc(4096);assert(p);memset(p,0x5b,4096);
        st=mi_uring_transfer(r,1,(mi_io_range){.data=p,.data_bytes=4096});
        int success=mode==0 || mode==3 || mode==5;
        assert(st==(success?MI_OK:MI_EIO));
        free(p); /* No in-flight request may reference caller memory after return. */
        mi_uring_destroy(r);
        if(mode==10) {
            /* The failed cancellation retained ring-owned DMA memory. Once the
             * kernel API recovers, the same owner can safely finish teardown. */
            mode=0;
            mi_uring_destroy(r);
            break;
        }
    }
    mode=0;assert(mi_uring_create(fd,2,&r)==MI_OK);
    assert(!mi_uring_register_buffer(r,-1,4096));
    char *fixed=mi_uring_buffer(r);assert(fixed);memset(fixed,0x4e,4096);
    assert(mi_uring_transfer_fixed(r,1,(mi_io_range){.data=fixed,.data_bytes=4096})==MI_OK);
    memset(fixed,0,4096);
    assert(mi_uring_transfer_fixed(r,0,(mi_io_range){.data=fixed,.data_bytes=4096})==MI_OK);
    for(unsigned i=0;i<4096;i++)assert(fixed[i]==0x4e);
    mode=4;assert(mi_uring_transfer_fixed(r,0,(mi_io_range){.data=fixed,.data_bytes=4096})==MI_EIO);
    assert(!mi_uring_buffer(r));mi_uring_destroy(r);
    mode=0;
    mi_storage_backend *disk;
    assert(mi_ssd_direct_backend_create(NULL,4096,16384,MI_SSD_VERIFY_ON_READ,1,-1,&disk)==MI_OK);
    if (!strcmp(mi_ssd_file_io_name(disk),"direct-uring-fixed")) {
        mi_storage_handle slot,another;
        assert(mi_backend_reserve(disk,4096,&slot)==MI_OK);
        mi_block_meta meta={.id=1,.version=1,.size=4096};char data[4096];memset(data,0x5a,sizeof(data));
        assert(mi_backend_write_record(disk,slot,&meta,data)==MI_OK);
        mode=4;
        assert(mi_backend_read_validated(disk,slot,&meta,data)==MI_EIO);
        assert(mi_backend_reserve(disk,4096,&another)==MI_EIO);
        assert(mi_backend_release(disk,slot)==MI_OK);
    }
    mi_backend_destroy(disk);
    close(fd);puts("uring SQE NULL, submit/wait EINTR/errors, CQE errors/zero, accepted submit failure: PASS");return 0;
}
