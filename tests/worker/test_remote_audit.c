#define _POSIX_C_SOURCE 200809L
#include "../../src/worker/remote/internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>

enum { BYTES = 4096, ROUNDS = 10000 };
static int delayed, acquisitions;
kvf_status __real_kvf_client_acquire_batch(kvf_client, kvf_hash, const kvf_hash *, uint32_t, kvf_grant *, uint32_t *);
kvf_status __wrap_kvf_client_acquire_batch(kvf_client c, kvf_hash space, const kvf_hash *p,
                                         uint32_t n, kvf_grant *g, uint32_t *matched) {
    if (delayed && ++acquisitions == 2)
        nanosleep(&(struct timespec){.tv_nsec=300000000}, NULL);
    return __real_kvf_client_acquire_batch(c, space, p, n, g, matched);
}
#ifdef AUDIT_RSOCKETS
/* Link-only rsocket stand-in exercises production fallback over real TCP.
 * No HCA is claimed: these are deterministic transport fault tests. */
static int injected, fake_fd = -1, rdma_bytes;
kvf_status __real_remote_connect(kvf_endpoint, kvf_transport, uint64_t, mi_remote_socket *);
kvf_status __wrap_remote_connect(kvf_endpoint ep, kvf_transport mode, uint64_t end, mi_remote_socket *out) {
    if (mode != KVF_TRANSPORT_RDMA_RSOCKET)
        return __real_remote_connect(ep, mode, end, out);
    if (injected == 1) return KVF_IO;
    kvf_status st = __real_remote_connect(ep, KVF_TRANSPORT_TCP, end, out);
    if (st == KVF_OK) {out->transport = mode;fake_fd = out->fd;rdma_bytes = 0;}
    return st;
}
ssize_t __wrap_rsend(int fd, const void *p, size_t n, int flags) {
    assert(fd == fake_fd);
    if (injected == 2) {errno = EPIPE;return -1;}
    return send(fd,p,n,flags|MSG_NOSIGNAL);
}
ssize_t __wrap_rrecv(int fd, void *p, size_t n, int flags) {
    assert(fd == fake_fd);
    if (injected == 3 || (injected == 4 && rdma_bytes >= 53)) {errno = ECONNRESET;return -1;}
    if (injected == 5) return 0;
    if (injected == 4 && rdma_bytes >= 36 && n > 17) n = 17;
    ssize_t got = recv(fd,p,n,flags);if(got > 0)rdma_bytes += (int)got;return got;
}
int __wrap_rclose(int fd) {assert(fd==fake_fd);fake_fd=-1;return close(fd);}
int __wrap_rpoll(struct pollfd *fds, nfds_t n, int timeout) {(void)timeout;return poll(fds,n,0);}
#endif
typedef struct { mi_mempool *pool; mi_storage_backend *cpu; mi_tiered_storage *storage; mi_remote_worker *worker; } fixture;
static fixture create(kvf_endpoint endpoint, kvf_identity id, const char *name) {
    fixture f={0};
    assert(mi_mempool_create(MI_MEMORY_CPU,-1,(mi_mempool_config){0},&f.pool)==MI_OK);
    assert(mi_memory_backend_create(MI_LOCATION_CPU,BYTES,f.pool,128,&f.cpu)==MI_OK);
    mi_storage_config cfg; mi_storage_config_default(&cfg);cfg.block_bytes=BYTES;cfg.cpu_bytes=128*BYTES;
    assert(mi_tiered_storage_create(cfg,(mi_storage_backends){.cpu=f.cpu},&f.storage)==MI_OK);
    mi_remote_config rc;mi_remote_config_defaults(&rc);rc.identity=id;rc.control=endpoint;rc.max_exports=64;rc.heartbeat_ms=1000;
    snprintf(rc.worker_name,sizeof(rc.worker_name),"%s",name);
    assert(mi_remote_worker_start(f.storage,rc,&f.worker)==KVF_OK);return f;
}
static void destroy(fixture f) {mi_remote_worker_stop(f.worker);mi_tiered_storage_destroy(f.storage);mi_backend_destroy(f.cpu);mi_mempool_destroy(f.pool);}
static mi_kv_sequence *page(fixture f, unsigned value) {
    mi_kv_sequence *s;assert(mi_kv_sequence_create(mi_remote_worker_cache(f.worker),&s)==MI_OK);
    assert(mi_remote_append_page(f.worker,s)==KVF_OK);
    mi_page_lease l;assert(mi_paged_kv_acquire(s,0,0,MI_ACCESS_WRITE,NULL,&l)==MI_OK);
    memset(l.lease.data,(int)value,BYTES);assert(mi_page_lease_finish(&l)==MI_OK);return s;
}
typedef struct {fixture f;kvf_client client;mi_kv_sequence *seq[2];kvf_page_key key;int32_t tokens[2];pthread_barrier_t gate;uint64_t old;kvf_grant grant;} race;
typedef struct {race *r;unsigned lane;} lane;
static void barrier(race *r) {int e=pthread_barrier_wait(&r->gate);assert(!e||e==PTHREAD_BARRIER_SERIAL_THREAD);}
static void *run(void *arg) {
    lane *l=arg;race *r=l->r;
    for (unsigned i=0;i<ROUNDS;i++) {
        if(l->lane==0) {
            assert(kvf_client_acquire(r->client,r->key,&r->grant)==KVF_OK);
            r->old=mi_remote_hide_page_revision(r->f.worker,r->key.page);assert(r->old);
        }
        barrier(r);
        if(l->lane==1) assert(mi_remote_publish_page(r->f.worker,r->seq[(i+1)%2],(kvf_hash){0},r->tokens)==KVF_OK);
        barrier(r);
        if(l->lane==2) {
            assert(mi_remote_invalidate_page_revision(r->f.worker,r->key.page,r->old)==KVF_STALE);
            assert(kvf_client_release(r->client,r->grant.ticket)==KVF_STALE);
        }
        if(l->lane==3) {
            kvf_grant g;assert(kvf_client_acquire(r->client,r->key,&g)==KVF_OK);
            mi_page_ref ref;assert(mi_kv_sequence_retain_page(r->seq[(i+1)%2],0,&ref)==MI_OK);
            assert(g.manifest.layer[0].id==mi_page_ref_block(&ref,0));mi_page_ref_release(&ref);
            assert(kvf_client_release(r->client,g.ticket)==KVF_OK);
        }
        barrier(r);
    }
    return NULL;
}
int main(void) {
    kvf_directory_config cfg;kvf_directory_defaults(&cfg);cfg.lease_ttl_ms=200;
    kvf_directory *d;assert(kvf_directory_create(cfg,&d)==KVF_OK);
    kvf_server *server;assert(kvf_control_start(d,(kvf_endpoint){{127,0,0,1},0},8,2000,&server)==KVF_OK);
    kvf_endpoint endpoint=kvf_server_endpoint(server);
    kvf_identity id={.tokens_per_page=2,.layers=1,.bytes_per_layer=BYTES,.dtype=KVF_F32,.tp_size=1,.pp_size=1};
    assert(kvf_hash_bytes("audit",5,&id.model)==KVF_OK);id.tokenizer=id.layout=id.tenant=id.model;
    fixture source=create(endpoint,id,"audit-source"),target=create(endpoint,id,"audit-target");
    race r={.f=source,.client={endpoint,2000},.tokens={700,701}};
    r.seq[0]=page(source,1);r.seq[1]=page(source,2);
    assert(kvf_identity_hash(&id,&r.key.space)==KVF_OK);assert(kvf_page_hash(&id,(kvf_hash){0},r.tokens,2,&r.key.page)==KVF_OK);
    /* Covers the old order=0 publication path as well as detached publication. */
    assert(mi_remote_publish(source.worker,r.seq[0],r.tokens,2,1)==KVF_OK);
    assert(pthread_barrier_init(&r.gate,NULL,4)==0);pthread_t t[4];lane lanes[4];
    for(unsigned i=0;i<4;i++){lanes[i]=(lane){&r,i};assert(!pthread_create(&t[i],NULL,run,&lanes[i]));}
    for(unsigned i=0;i<4;i++)
        assert(!pthread_join(t[i],NULL));
    pthread_barrier_destroy(&r.gate);
    mi_remote_hide_page(source.worker,r.key.page);assert(mi_remote_invalidate_page(source.worker,r.key.page)==KVF_OK);
    for(unsigned i=0;i<2;i++)mi_kv_sequence_destroy(r.seq[i]);
    int32_t tokens[18];mi_kv_sequence *all;assert(mi_kv_sequence_create(mi_remote_worker_cache(source.worker),&all)==MI_OK);
    for(unsigned i=0;i<9;i++) {
        tokens[2*i]=100+(int)i;tokens[2*i+1]=200+(int)i;
        assert(mi_remote_append_page(source.worker,all)==KVF_OK);mi_page_lease l;
        assert(mi_paged_kv_acquire(all,i,0,MI_ACCESS_WRITE,NULL,&l)==MI_OK);memset(l.lease.data,(int)i,BYTES);assert(mi_page_lease_finish(&l)==MI_OK);
    }
    assert(mi_remote_publish(source.worker,all,tokens,18,9)==KVF_OK);mi_kv_sequence_destroy(all);
    delayed=1;mi_remote_result result;mi_kv_sequence *imported;
    assert(mi_remote_resolve_after(target.worker,(mi_remote_suffix){.deadline_ms=kvf_now_ms()+2000},tokens,18,&imported,&result,NULL)==KVF_OK);
    assert(result.remote_status==KVF_OK);
    assert(acquisitions==2 && result.remote_tokens==18 && result.valid_until_ms==0);
    for(unsigned i=0;i<9;i++){mi_page_lease l;assert(mi_paged_kv_acquire(imported,i,0,MI_ACCESS_READ,NULL,&l)==MI_OK);for(unsigned j=0;j<BYTES;j++)assert(((unsigned char *)l.lease.data)[j]==i);assert(mi_page_lease_finish(&l)==MI_OK);}
    mi_kv_sequence_destroy(imported);kvf_directory_stats stats;kvf_directory_get_stats(d,&stats);assert(stats.leases==0);
#ifdef AUDIT_RSOCKETS
    /* Use an isolated importer for the link-injected transport. Startup
     * configuration remains immutable in the running worker/service threads. */
    mi_remote_worker faults = {.storage=target.storage,.cache=target.worker->cache,
        .control=target.worker->control,.identity=id,.location=MI_LOCATION_CPU,
        .transport_mode=KVF_TRANSPORT_RDMA_RSOCKET,.device=-1,.peers=target.worker->peers};
    atomic_init(&faults.active_transport,KVF_TRANSPORT_RDMA_RSOCKET);
    atomic_init(&faults.warned_peer_fallback,0);
    kvf_hash hash;assert(kvf_page_hash(&id,(kvf_hash){0},tokens,2,&hash)==KVF_OK);
    kvf_grant g;assert(kvf_client_acquire(r.client,(kvf_page_key){r.key.space,hash},&g)==KVF_OK);
    for (injected = 1; injected <= 5; ++injected) {
        mi_kv_sequence *s;assert(mi_kv_sequence_create(mi_remote_worker_cache(target.worker),&s)==MI_OK);
        assert(mi_remote_append_page(target.worker,s)==KVF_OK);
        assert(mi_remote_import(&faults,s,0,&g,kvf_now_ms()+2000,NULL)==KVF_OK);
        mi_page_lease l;assert(mi_paged_kv_acquire(s,0,0,MI_ACCESS_READ,NULL,&l)==MI_OK);
        for (unsigned j=0;j<BYTES;j++)assert(((unsigned char *)l.lease.data)[j]==0);
        assert(mi_page_lease_finish(&l)==MI_OK);mi_kv_sequence_destroy(s);
    }
    injected=4;kvf_batch_page bp={.key=g.manifest.key,.owner_boot=g.manifest.owner.boot,.ticket=g.ticket};
    assert(kvf_manifest_digest(&g.manifest,&bp.manifest_digest)==KVF_OK);mi_remote_timing timing={0};
    assert(mi_remote_receive_batch(&faults,&g,&bp,1,BYTES,kvf_now_ms()+2000,NULL,&timing)==KVF_IO);
    assert(timing.received_bytes==17); /* partial body never reaches page commit */
    assert(kvf_client_release(r.client,g.ticket)==KVF_OK);
    free(faults.batch_recv.data);
#endif
    destroy(target);destroy(source);kvf_server_stop(server);kvf_directory_destroy(d);
    puts("4 threads x 10000 generation races; expired finished batch + live second batch: PASS");return 0;
}
