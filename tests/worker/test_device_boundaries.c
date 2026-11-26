#include "miniinfer/device_transfer.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void *transfer(void *arg) {
    int d=*(int *)arg;const size_t bytes=65536;unsigned char *host,*out;void *gpu;
    assert(mi_device_host_alloc(d,bytes,(void **)&host)==MI_OK);assert(mi_device_host_alloc(d,bytes,(void **)&out)==MI_OK);
    assert(mi_device_alloc(d,bytes,&gpu)==MI_OK);mi_stream stream;mi_event event;
    assert(mi_device_stream_create(d,&stream)==MI_OK);assert(mi_device_event_create(d,&event)==MI_OK);
    for(unsigned i=0;i<100;i++) {
        memset(host,(int)i,bytes);assert(mi_device_copy_from_host(d,gpu,host,bytes,stream)==MI_OK);
        assert(mi_device_copy_to_host(d,out,gpu,bytes,stream)==MI_OK);
        assert(mi_device_event_record(d,event,stream)==MI_OK);assert(mi_device_event_wait(d,event)==MI_OK);
        assert(!memcmp(host,out,bytes));
    }
    assert(mi_device_stream_wait(d,stream)==MI_OK);assert(mi_device_event_destroy(d,event)==MI_OK);assert(mi_device_stream_destroy(d,stream)==MI_OK);
    assert(mi_device_free(d,gpu)==MI_OK);assert(mi_device_host_free(d,host)==MI_OK);assert(mi_device_host_free(d,out)==MI_OK);return NULL;
}
int main(int argc,char **argv) {
    (void)argv;void *p;assert(mi_device_alloc(-1,1,NULL)==MI_EINVAL);assert(mi_device_host_alloc(-1,0,&p)==MI_EINVAL);
    assert(mi_device_stream_create(-1,NULL)==MI_EINVAL);assert(mi_device_event_create(-1,NULL)==MI_EINVAL);
    assert(mi_device_copy_from_host(-1,NULL,NULL,0,NULL)==MI_OK);assert(mi_device_copy_to_host(-1,NULL,NULL,0,NULL)==MI_OK);
    assert(mi_device_copy_from_host(-1,NULL,"x",1,NULL)==MI_EINVAL);
    assert(mi_device_stream_destroy(-1,(mi_stream)(uintptr_t)1)==MI_OK);
    assert(mi_device_event_destroy(-1,(mi_event)(uintptr_t)1)==MI_OK);
    int device=argc>1?0:-1;if(device>=0 && mi_device_select(device)!=MI_OK)return 77;
    pthread_t threads[4];for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,transfer,&device));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    puts("device boundaries + 4 streams x 100 bidirectional copies: PASS");return 0;
}
