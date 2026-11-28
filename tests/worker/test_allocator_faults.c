#include "miniinfer/allocator.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static long ordinal=-1,calls;
void *__real_calloc(size_t,size_t);void *__real_malloc(size_t);
void *__wrap_calloc(size_t n,size_t m){if(ordinal>=0 && calls++==ordinal)return NULL;return __real_calloc(n,m);}
void *__wrap_malloc(size_t n){if(ordinal>=0 && calls++==ordinal)return NULL;return __real_malloc(n);}
int main(void) {
    unsigned failures=0;
    for(long i=0;i<24;i++) {
        mi_mempool *pool;assert(mi_mempool_create(MI_MEMORY_CPU,-1,(mi_mempool_config){.alignment=64,
            .chunk_bytes=8192,.max_reserved_bytes=8192,.max_cached_bytes=8192},&pool)==MI_OK);
        ordinal=i;calls=0;void *p;mi_status st=mi_mempool_alloc(pool,1024,0,&p);ordinal=-1;
        if(st==MI_OK)assert(mi_mempool_release(pool,p)==MI_OK);else{assert(st==MI_ENOMEM);failures++;}
        assert(mi_mempool_alloc(pool,8192,0,&p)==MI_OK);assert(mi_mempool_release(pool,p)==MI_OK);mi_mempool_destroy(pool);
    }
    assert(failures>0);printf("allocator metadata allocation failure recovery: %u points PASS\n",failures);return 0;
}
