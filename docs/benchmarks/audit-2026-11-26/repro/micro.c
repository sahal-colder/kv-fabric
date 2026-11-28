#define _POSIX_C_SOURCE 200809L
#include "miniinfer/miniinfer.h"
#include "kvfabric/client.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
static double ns(void) {struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t)); return t.tv_sec*1e9+t.tv_nsec;}
int main(int argc,char **argv) {
 if(argc==2) {kvf_client c={.timeout_ms=2000}; assert(kvf_endpoint_parse(argv[1],&c.endpoint)==KVF_OK); kvf_directory_stats s; for(int r=0;r<7;r++){double a=ns();for(int i=0;i<1000;i++)assert(kvf_client_stats(c,&s)==KVF_OK);printf("directory_rpc,%d,%.3f,ns\n",r,(ns()-a)/1000);}return 0;}
 mi_mempool *p; const size_t cap=8*1024*1024;assert(mi_mempool_create(MI_MEMORY_CPU,-1,(mi_mempool_config){.alignment=64,.chunk_bytes=cap,.max_reserved_bytes=cap,.max_cached_bytes=cap},&p)==MI_OK);
 for(int r=0;r<7;r++){void *v[128];double a=ns(); for(int k=0;k<2000;k++){for(int i=0;i<128;i++)assert(mi_mempool_alloc(p,(i%31+1)*64,0,&v[i])==MI_OK);for(int i=0;i<128;i++)assert(mi_mempool_release(p,v[i])==MI_OK);}double d=ns()-a;printf("tlsf_pair,%d,%.3f,ns\n",r,d/(2000*128));}void *whole;assert(mi_mempool_alloc(p,cap,0,&whole)==MI_OK); assert(mi_mempool_release(p,whole)==MI_OK); puts("tlsf_after_coalescing_external_fragmentation,0,0,ratio");mi_mempool_destroy(p);
 mi_storage_backend *cpu;mi_tiered_storage *s;mi_paged_kv *kv;mi_storage_config c;assert(mi_mempool_create(MI_MEMORY_CPU,-1,(mi_mempool_config){},&p)==MI_OK);assert(mi_memory_backend_create(MI_LOCATION_CPU,4096,p,256,&cpu)==MI_OK);mi_storage_config_default(&c);c.block_bytes=4096;c.cpu_bytes=256*4096;c.gpu_bytes=c.ssd_bytes=0;assert(mi_tiered_storage_create(c,(mi_storage_backends){.cpu=cpu},&s)==MI_OK);assert(mi_paged_kv_create(s,2,4,4096,"audit-bench",&kv)==MI_OK);
 for(int r=0;r<7;r++){double a=ns();for(int i=0;i<100000;i++){mi_kv_sequence *q;assert(mi_kv_sequence_create(kv,&q)==MI_OK);assert(mi_kv_sequence_append_page(q,MI_LOCATION_CPU)==MI_OK);mi_kv_sequence_destroy(q);}printf("page_pair,%d,%.3f,ns\n",r,(ns()-a)/100000);}mi_paged_kv_destroy(kv);mi_tiered_storage_destroy(s);mi_backend_destroy(cpu);mi_mempool_destroy(p);return 0;
}
