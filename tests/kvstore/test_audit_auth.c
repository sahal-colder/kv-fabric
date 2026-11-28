#define _POSIX_C_SOURCE 200809L
#include "kvs_transport.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static const char secret[]="unit-test-replication-secret-32-bytes";
typedef struct {kvs_transport_t io;const char *key;int result;} server;
static void *authenticate(void *arg){server *s=arg;s->result=kvs_transport_authenticate(&s->io,s->key,1,500);kvs_transport_close(&s->io);return NULL;}
#if KVS_ENABLE_RDMA
static unsigned probes;
int __wrap_rpoll(struct pollfd *p,nfds_t n,int timeout){(void)p;(void)n;assert(timeout==0);probes++;if(probes<4){errno=EINTR;return -1;}return 0;}
#endif
int main(void) {
    for(int wrong=0;wrong<2;wrong++) {
        int fds[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));server s={.io={fds[0],0},.key=secret};pthread_t t;
        assert(!pthread_create(&t,NULL,authenticate,&s));kvs_transport_t client={fds[1],0};
        int result=kvs_transport_authenticate(&client,wrong?"different-test-secret-at-least-32-bytes":secret,0,500);
        kvs_transport_close(&client);assert(!pthread_join(t,NULL));assert((result==0)==!wrong && (s.result==0)==!wrong);
    }
#if KVS_ENABLE_RDMA
    kvs_transport_t fake={7,1};struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
    assert(kvs_transport_wait(&fake,POLLIN,15)<0 && errno==ETIMEDOUT);clock_gettime(CLOCK_MONOTONIC,&b);
    double ms=(b.tv_sec-a.tv_sec)*1000.0+(b.tv_nsec-a.tv_nsec)/1e6;assert(ms>=14 && ms<500 && probes>4);
    puts("zero-time rpoll + EINTR preserves absolute timeout: PASS");
#endif
    puts("replication mutual HMAC authentication + wrong-key rejection: PASS");return 0;
}
