#include "kvs_store.h"
#include "kvs_allocator.h"
#include "kvs_resp.h"
#include "kvs_connection.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static long fail_at=-1,calls;
void *__real_kvs_malloc(size_t);void *__real_kvs_calloc(size_t,size_t);void *__real_kvs_realloc(void *,size_t);
static int fail(void){return fail_at>=0 && calls++==fail_at;}
void *__wrap_kvs_malloc(size_t n){return fail()?NULL:__real_kvs_malloc(n);}
void *__wrap_kvs_calloc(size_t n,size_t m){return fail()?NULL:__real_kvs_calloc(n,m);}
void *__wrap_kvs_realloc(void *p,size_t n){return fail()?NULL:__real_kvs_realloc(p,n);}
int main(void) {
    assert(kvs_allocator_init()==0);
    const char *keys[]={"existing","new","existing"},*values[]={"first","new-value","last"};
    for(int engine=0;engine<4;engine++) {
        int failures=0,success=0;
        for(long ordinal=0;ordinal<128;ordinal++) {
            kvs_store_t store;assert(!kvs_store_create(&store));
            assert(!kvs_store_mutate(&store,engine,KVS_INSERT,"existing","original"));
            fail_at=ordinal;calls=0;int result=kvs_store_mset(&store,engine,keys,values,3,0);fail_at=-1;
            if(result) {
                failures++;assert(!strcmp(kvs_store_get(&store,engine,"existing"),"original"));
                assert(!kvs_store_get(&store,engine,"new"));assert(kvs_store_count(&store,engine)==1);
            } else {
                success=1;assert(!strcmp(kvs_store_get(&store,engine,"existing"),"last"));
                assert(!strcmp(kvs_store_get(&store,engine,"new"),"new-value"));assert(kvs_store_count(&store,engine)==2);
                assert(kvs_store_mset(&store,engine,keys,values,3,1)==1);
            }
            kvs_store_destroy(&store);if(success)break;
        }
        assert(failures>0 && success);
        printf("engine %d: all %d preparation allocation failures atomic; duplicate keys/NX PASS\n",engine,failures);
    }
    kvs_store_t store;assert(!kvs_store_create(&store));char key[32];
    for(int i=0;i<KVS_ARRAY_SIZE-1;i++){snprintf(key,sizeof(key),"key-%d",i);assert(!kvs_array_set(&store.array,key,"v"));}
    const char *more[]={"one","two"},*v[]={"v1","v2"};
    assert(kvs_store_mset(&store,0,more,v,2,0)<0);assert(!kvs_array_get(&store.array,"one") && !kvs_array_get(&store.array,"two"));
    kvs_store_destroy(&store);
    char small[1024];size_t used=0;assert(kvs_resp_append_bulk(small,&used,"x",SIZE_MAX)<0 && !used);
    used=SIZE_MAX;assert(kvs_resp_append_bulk(small,&used,"x",1)<0 && used==SIZE_MAX);
    kvs_resp_buffer reply={.data=small,.capacity=sizeof(small),.grow=1};char *large=malloc(32768);assert(large);memset(large,'a',32768);
    assert(!kvs_resp_buffer_bulk(&reply,large,32768));assert(reply.owned && reply.used==32778);assert(!memcmp(reply.data,"$32768\r\n",8));
    assert(!memcmp(reply.data+8,large,32768));kvs_free(reply.data);free(large);
    kvs_connection_t conn={.wlength=SIZE_MAX};assert(kvs_connection_queue_output(&conn,"xx",2)<0);
    kvs_allocator_destroy();
    puts("array capacity rollback; RESP integer overflow + 32KiB bulk + connection overflow: PASS");return 0;
}
