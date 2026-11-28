#define _POSIX_C_SOURCE 200809L
#include "kvfabric/client.h"
#include "miniinfer/miniinfer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static int tlsf(void) {
 mi_mempool *p;const size_t u=64,c=(67+1+66+1)*u;void *a,*b,*g[2],*out;
 if(mi_mempool_create(MI_MEMORY_CPU,-1,(mi_mempool_config){.alignment=u,.chunk_bytes=c,.max_reserved_bytes=c,.max_cached_bytes=c},&p)!=MI_OK)return 2;
 if(mi_mempool_alloc(p,67*u,0,&a)||mi_mempool_alloc(p,u,0,&g[0])||mi_mempool_alloc(p,66*u,0,&b)||mi_mempool_alloc(p,u,0,&g[1]))return 3;
 mi_mempool_release(p,a);mi_mempool_release(p,b);mi_status st=mi_mempool_alloc(p,67*u,0,&out);printf("same-bin available 4288-byte block, alloc returned %s\n",mi_status_string(st));mi_mempool_destroy(p);return st==MI_ENOMEM?0:1;
}
static int batch(void) {
 kvf_directory_config cfg;kvf_directory_defaults(&cfg);kvf_directory *d;
 if(kvf_directory_create(cfg,&d))return 2;
 kvf_server *server;if(kvf_control_start(d,(kvf_endpoint){{127,0,0,1},0},2,1000,&server))return 3;
 kvf_client c={kvf_server_endpoint(server),1000};kvf_worker_info worker={.endpoint={{127,0,0,1},9001},.capacity_pages=32};memset(worker.owner.name,'x',KVF_WORKER_NAME-1);kvf_uuid_new(&worker.owner.boot);
 if(kvf_client_register(c,&worker,(kvf_uuid){0}))return 4;
 kvf_manifest m={.owner=worker.owner,.token_count=2,.layer_count=128,.identity={.tokens_per_page=2,.layers=128,.bytes_per_layer=4096,.dtype=KVF_F32,.tp_size=1,.pp_size=1}};
 kvf_hash_bytes("audit",5,&m.identity.model);m.identity.tokenizer=m.identity.layout=m.identity.tenant=m.identity.model;kvf_identity_hash(&m.identity,&m.key.space);
 for(unsigned i=0;i<128;i++)m.layer[i]=(kvf_layer){.id=i+1,.version=1,.size=4096,.checksum=17};kvf_hash pages[8];
 for(unsigned i=0;i<8;i++){int32_t tok[]={1,(int32_t)i};kvf_page_hash(&m.identity,m.parent,tok,2,&m.key.page);pages[i]=m.key.page;if(kvf_client_publish(c,&m))return 5;m.parent=m.key.page;}
 kvf_grant grants[8];uint32_t matched=0;kvf_status st=kvf_client_acquire_batch(c,m.key.space,pages,8,grants,&matched);
 printf("8 x 128 layers, max worker name: acquire_batch=%s, matched=%u\n",kvf_status_string(st),matched);
 kvf_server_stop(server);kvf_directory_destroy(d);return st!=KVF_OK?0:1;
}
int main(int argc,char **argv){if(argc!=2)return 2;if(!strcmp(argv[1],"null")){printf("owner_valid(NULL)=%d\n",kvf_owner_valid(NULL));return 1;}return !strcmp(argv[1],"tlsf")?tlsf():batch();}
