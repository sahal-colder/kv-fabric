/* Explicit Samsung scratch-region experiment. Not the production tiered path.
 * Both backends use the same hugepage + CUDA-registered host buffer, record
 * layout, ordered submissions, CRC, and H2D. Optional pipeline mode overlaps
 * reading later records with CRC/H2D of completed records. Only prepare writes. */
#define _GNU_SOURCE
#include <cuda_runtime_api.h>
#include <liburing.h>
#include <spdk/bdev.h>
#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/nvme_spec.h>
#include <spdk/thread.h>
#include "kvfabric/kvfabric.h"
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

enum { PAYLOAD=524288, HEADER=4096, STRIDE=PAYLOAD+HEADER, COUNT=32,
       REGION=134217728, HOST_BYTES=18*1024*1024 };
static const uint64_t offset=1073741824;
typedef struct { uint64_t magic, format, id, version, bytes; uint32_t crc; } record;
#define MAGIC UINT64_C(0x314b5646504d4153)
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
#define CUDA(x) do { cudaError_t e_=(x); if(e_!=cudaSuccess) { fprintf(stderr,"CUDA: %s\n",cudaGetErrorString(e_)); exit(1); } } while(0)
static double stamp(clockid_t clock) { struct timespec t; REQUIRE(!clock_gettime(clock,&t)); return t.tv_sec+t.tv_nsec*1e-9; }
static void pin(int cpu) { cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu,&set); REQUIRE(!sched_setaffinity(0,sizeof(set),&set)); }
static void pattern(unsigned char *p,unsigned id) { for(size_t i=0;i<PAYLOAD;i++) p[i]=(unsigned char)((i*61u)^(i>>8)^(id*17u)); }
static unsigned char *host;
static struct {
    pthread_t reactor;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_cond_t records_changed, work_available;
    struct spdk_bdev_desc *desc;
    struct spdk_io_channel *ch;
    struct spdk_thread *thread;
    struct spdk_poller *stop_poller;
    atomic_bool stop;
    unsigned next,active,completed,peak,qd;
    int done,error,temp,pipeline,pending,fd;
    unsigned ready[COUNT], records_completed;
    double storage_done;
    struct io_uring *ring;
    void *health;
} s;
static void notify_done(void) { pthread_mutex_lock(&s.lock); s.done=1; pthread_cond_signal(&s.changed); pthread_mutex_unlock(&s.lock); }
static void pump(void);
static void publish_record(unsigned i) {
    pthread_mutex_lock(&s.lock);
    REQUIRE(i<COUNT && !s.ready[i]); s.ready[i]=1;
    if(++s.records_completed==COUNT)s.storage_done=stamp(CLOCK_MONOTONIC);
    pthread_cond_broadcast(&s.records_changed);
    pthread_mutex_unlock(&s.lock);
}
static void read_done(struct spdk_bdev_io *io,bool ok,void *arg) {
    unsigned i=(unsigned)(uintptr_t)arg;
    spdk_bdev_free_io(io); REQUIRE(ok);
    --s.active; ++s.completed;
    if(s.pipeline)publish_record(i);
    pump();
}
static void pump(void) {
    while(!s.error && s.next<COUNT && s.active<s.qd) {
        unsigned i=s.next++;
        int rc=spdk_bdev_read(s.desc,s.ch,host+(size_t)i*STRIDE,offset+(size_t)i*STRIDE,STRIDE,read_done,(void*)(uintptr_t)i);
        if(rc) { s.error=rc; break; }
        ++s.active; if(s.active>s.peak)s.peak=s.active;
    }
    if(!s.active && (s.error || s.completed==COUNT))notify_done();
}
static void begin_reads(void *arg) { (void)arg; s.next=s.active=s.completed=s.peak=0; s.error=0; pump(); }
static void health_done(struct spdk_bdev_io *io,bool ok,void *arg) {
    (void)arg; spdk_bdev_free_io(io);
    unsigned char *p=s.health;
    s.temp=ok ? (int)(p[1]|p[2]<<8)-273 : -999;
    notify_done();
}
static void get_health(void *arg) {
    (void)arg;
    struct spdk_nvme_cmd cmd={0};
    cmd.opc=SPDK_NVME_OPC_GET_LOG_PAGE; cmd.nsid=0xffffffff;
    cmd.cdw10=SPDK_NVME_LOG_HEALTH_INFORMATION | (127u<<16);
    if(spdk_bdev_nvme_admin_passthru(s.desc,s.ch,&cmd,s.health,512,health_done,NULL)) { s.temp=-999; notify_done(); }
}
static void message(spdk_msg_fn fn) {
    pthread_mutex_lock(&s.lock); s.done=0;
    REQUIRE(!spdk_thread_send_msg(s.thread,fn,NULL));
    while(!s.done)pthread_cond_wait(&s.changed,&s.lock);
    pthread_mutex_unlock(&s.lock);
}
static void event(enum spdk_bdev_event_type type,struct spdk_bdev *b,void *arg) { (void)b;(void)arg; REQUIRE(type!=SPDK_BDEV_EVENT_REMOVE); }
static int stop_poll(void *arg) {
    (void)arg; if(!atomic_load(&s.stop))return SPDK_POLLER_IDLE;
    spdk_poller_unregister(&s.stop_poller); spdk_put_io_channel(s.ch); spdk_bdev_close(s.desc);
    spdk_dma_free(s.health); REQUIRE(!spdk_mem_unregister(host,HOST_BYTES)); spdk_app_stop(0);
    return SPDK_POLLER_BUSY;
}
static void start(void *arg) {
    (void)arg;
    REQUIRE(!spdk_bdev_open_ext("Samsungn1",true,event,NULL,&s.desc));
    struct spdk_bdev *b=spdk_bdev_desc_get_bdev(s.desc);
    REQUIRE(spdk_bdev_get_num_blocks(b)*spdk_bdev_get_block_size(b)>=offset+REGION);
    s.ch=spdk_bdev_get_io_channel(s.desc); REQUIRE(s.ch);
    REQUIRE(!spdk_mem_register(host,HOST_BYTES));
    s.health=spdk_dma_zmalloc(4096,4096,NULL); REQUIRE(s.health);
    s.thread=spdk_get_thread(); s.stop_poller=spdk_poller_register(stop_poll,NULL,1000); REQUIRE(s.stop_poller);
    notify_done();
}
static void *reactor(void *arg) {
    struct spdk_app_opts o; spdk_app_opts_init(&o,sizeof(o));
    o.name="kvf_backend_compare"; o.json_config_file=arg; o.reactor_mask="0x4";
    o.rpc_addr=NULL; o.disable_signal_handlers=true; o.iova_mode="va";
    o.base_virtaddr=UINT64_C(0x1000000000); o.mem_size=256; o.num_entries=0;
    o.env_context="--legacy-mem";
    REQUIRE(!spdk_app_start(&o,start,NULL)); spdk_app_fini(); return NULL;
}
static int kernel_temp(const char *device) {
    char path[256]; const char *name=strrchr(device,'/'); REQUIRE(name); ++name;
    char controller[64]; snprintf(controller,sizeof(controller),"%s",name);
    char *ns=strchr(controller+4,'n'); REQUIRE(ns); *ns=0;
    for(int i=0;i<64;i++) {
        snprintf(path,sizeof(path),"/sys/class/nvme/%s/hwmon%d/temp1_input",controller,i);
        FILE *f=fopen(path,"r"); if(f) { int m; REQUIRE(fscanf(f,"%d",&m)==1); fclose(f); return m/1000; }
    }
    return -999;
}
static unsigned uring_read(struct io_uring *r,int fd,unsigned qd) {
    unsigned next=0,active=0,done=0,peak=0;
    while(done<COUNT) {
        unsigned added=0;
        while(next<COUNT && active<qd) {
            struct io_uring_sqe *sqe=io_uring_get_sqe(r); REQUIRE(sqe);
            unsigned i=next++;
            io_uring_prep_read_fixed(sqe,fd,host+(size_t)i*STRIDE,STRIDE,offset+(size_t)i*STRIDE,0);
            io_uring_sqe_set_data64(sqe,i); ++active; ++added;
        }
        if(active>peak)peak=active;
        if(added)REQUIRE(io_uring_submit(r)==(int)added);
        struct io_uring_cqe *cqe; REQUIRE(!io_uring_wait_cqe(r,&cqe)); REQUIRE(cqe->res==STRIDE);
        unsigned i=(unsigned)io_uring_cqe_get_data64(cqe);
        io_uring_cqe_seen(r,cqe); --active; ++done;
        if(s.pipeline)publish_record(i);
    }
    return peak;
}
static void *uring_worker(void *arg) {
    (void)arg; pin(2);
    for(;;) {
        pthread_mutex_lock(&s.lock);
        while(!s.pending && !atomic_load(&s.stop))pthread_cond_wait(&s.work_available,&s.lock);
        int stop=atomic_load(&s.stop); s.pending=0;
        pthread_mutex_unlock(&s.lock);
        if(stop)break;
        s.peak=uring_read(s.ring,s.fd,s.qd); s.completed=COUNT; notify_done();
    }
    return NULL;
}
static void launch_pipeline(int spdk) {
    pthread_mutex_lock(&s.lock);
    s.done=0; s.records_completed=0; memset(s.ready,0,sizeof(s.ready));
    if(spdk)REQUIRE(!spdk_thread_send_msg(s.thread,begin_reads,NULL));
    else { s.pending=1; pthread_cond_signal(&s.work_available); }
    pthread_mutex_unlock(&s.lock);
}
static void wait_record(unsigned i) {
    pthread_mutex_lock(&s.lock);
    while(!s.ready[i])pthread_cond_wait(&s.records_changed,&s.lock);
    pthread_mutex_unlock(&s.lock);
}
static void wait_pipeline(void) {
    pthread_mutex_lock(&s.lock);
    while(!s.done)pthread_cond_wait(&s.changed,&s.lock);
    pthread_mutex_unlock(&s.lock);
    REQUIRE(!s.error && s.completed==COUNT && s.records_completed==COUNT);
}
static void verify_record(unsigned i) {
    unsigned char *p=host+(size_t)i*STRIDE; record h; memcpy(&h,p,sizeof(h));
    REQUIRE(h.magic==MAGIC&&h.format==1&&h.id==i+1&&h.version==1&&h.bytes==PAYLOAD);
    REQUIRE(h.crc==kvf_crc32c(p+HEADER,PAYLOAD));
}
int main(int argc,char **argv) {
    if(argc!=9 && argc!=10) { fprintf(stderr,"usage: %s prepare|uring|spdk DEVICE|CONFIG QD cpu|gpu CSV ROUNDS WARMUP BATCH [serial|pipeline]\n",argv[0]); return 2; }
    int prepare=!strcmp(argv[1],"prepare"),spdk=!strcmp(argv[1],"spdk"),gpu=!strcmp(argv[4],"gpu");
    unsigned qd=(unsigned)atoi(argv[3]),rounds=(unsigned)atoi(argv[6]),warmup=(unsigned)atoi(argv[7]);
    REQUIRE((prepare||spdk||!strcmp(argv[1],"uring")) && qd>=1 && qd<=COUNT && !(qd&(qd-1)) && rounds>=30 && rounds<=1000 && warmup>=3 && warmup<=100);
    s.pipeline=argc==10 && !strcmp(argv[9],"pipeline"); s.qd=qd;
    REQUIRE(argc==9 || s.pipeline || !strcmp(argv[9],"serial"));
    REQUIRE(!strcmp(argv[4],"cpu")||gpu); pin(4);
    host=mmap((void*)UINT64_C(0x1800000000),HOST_BYTES,PROT_READ|PROT_WRITE,
              MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB|MAP_FIXED_NOREPLACE,-1,0);
    REQUIRE(host!=MAP_FAILED); REQUIRE(!mlock(host,HOST_BYTES));
    unsigned char *expected=malloc((size_t)COUNT*STRIDE),*actual=malloc((size_t)COUNT*PAYLOAD); REQUIRE(expected&&actual);
    memset(expected,0,(size_t)COUNT*STRIDE);
    for(unsigned i=0;i<COUNT;i++) {
        unsigned char *p=expected+(size_t)i*STRIDE; pattern(p+HEADER,i);
        record h={.magic=MAGIC,.format=1,.id=i+1,.version=1,.bytes=PAYLOAD,.crc=kvf_crc32c(p+HEADER,PAYLOAD)};
        memcpy(p,&h,sizeof(h));
    }
    int fd=-1; struct io_uring ring;
    if(!spdk) {
        fd=open(argv[2],(prepare?O_RDWR:O_RDONLY)|O_DIRECT|O_CLOEXEC); REQUIRE(fd>=0);
        struct stat st; REQUIRE(!fstat(fd,&st)&&S_ISBLK(st.st_mode));
        char path[256],serial[128];
        snprintf(path,sizeof(path),"/sys/dev/block/%u:%u/device/serial",major(st.st_rdev),minor(st.st_rdev));
        FILE *identity=fopen(path,"r"); REQUIRE(identity);
        REQUIRE(fscanf(identity,"%127s",serial)==1); fclose(identity);
        REQUIRE(!strcmp(serial,"S4Y4NX0R755035"));
    }
    if(prepare) {
        memcpy(host,expected,(size_t)COUNT*STRIDE);
        REQUIRE(pwrite(fd,host,(size_t)COUNT*STRIDE,offset)==(ssize_t)COUNT*STRIDE); REQUIRE(!fsync(fd));
        memset(host,0,(size_t)COUNT*STRIDE);
        REQUIRE(pread(fd,host,(size_t)COUNT*STRIDE,offset)==(ssize_t)COUNT*STRIDE);
        REQUIRE(!memcmp(host,expected,(size_t)COUNT*STRIDE)); puts("prepare_fixed_records=PASS"); close(fd); return 0;
    }
    CUDA(cudaSetDevice(0)); CUDA(cudaHostRegister(host,HOST_BYTES,cudaHostRegisterDefault));
    unsigned char *device=NULL; cudaStream_t stream;
    CUDA(cudaStreamCreate(&stream)); CUDA(cudaMalloc((void**)&device,(size_t)COUNT*PAYLOAD));
    CUDA(cudaMemset(device,0,(size_t)COUNT*PAYLOAD)); CUDA(cudaDeviceSynchronize());
    pthread_mutex_init(&s.lock,NULL); pthread_cond_init(&s.changed,NULL);
    pthread_cond_init(&s.records_changed,NULL); pthread_cond_init(&s.work_available,NULL);
    atomic_init(&s.stop,false);
    if(spdk) {
        pthread_mutex_lock(&s.lock); REQUIRE(!pthread_create(&s.reactor,NULL,reactor,argv[2]));
        while(!s.done)pthread_cond_wait(&s.changed,&s.lock);
        pthread_mutex_unlock(&s.lock);
    } else {
        REQUIRE(!io_uring_queue_init(64,&ring,0)); struct iovec v={host,HOST_BYTES}; REQUIRE(!io_uring_register_buffers(&ring,&v,1));
        if(s.pipeline) { s.ring=&ring; s.fd=fd; REQUIRE(!pthread_create(&s.reactor,NULL,uring_worker,NULL)); }
    }
    FILE *out=fopen(argv[5],"wx"); REQUIRE(out);
    fprintf(out,"backend,target,batch,round,warmup,qd,max_inflight,submitted,completed,payload_bytes,read_ms,crc_ms,h2d_ms,total_ms,cpu_ms,read_cpu_ms,temp_before_c,temp_after_c,verified_bytes,strategy,overlapped,wait_ms,h2d_submit_ms,final_sync_ms\n");
    for(unsigned n=0;n<warmup+rounds;n++) {
        memset(host,0xa5,(size_t)COUNT*STRIDE);
        int before,after; if(spdk) { message(get_health); before=s.temp; } else before=kernel_temp(argv[2]);
        double c0=stamp(CLOCK_PROCESS_CPUTIME_ID),a=stamp(CLOCK_MONOTONIC);
        unsigned peak; double read_ms,crc_ms=0,h2d_ms=0,wait_ms=0,submit_ms=0,sync_ms=0,c1;
        if(s.pipeline) {
            launch_pipeline(spdk);
            for(unsigned i=0;i<COUNT;i++) {
                double w=stamp(CLOCK_MONOTONIC); wait_record(i); double b=stamp(CLOCK_MONOTONIC); wait_ms+=(b-w)*1000;
                verify_record(i); double v=stamp(CLOCK_MONOTONIC); crc_ms+=(v-b)*1000;
                if(gpu)CUDA(cudaMemcpyAsync(device+(size_t)i*PAYLOAD,host+(size_t)i*STRIDE+HEADER,PAYLOAD,cudaMemcpyHostToDevice,stream));
                submit_ms+=(stamp(CLOCK_MONOTONIC)-v)*1000;
            }
            wait_pipeline(); c1=stamp(CLOCK_PROCESS_CPUTIME_ID); peak=s.peak;
            double v=stamp(CLOCK_MONOTONIC);
            if(gpu)CUDA(cudaStreamSynchronize(stream));
            sync_ms=(stamp(CLOCK_MONOTONIC)-v)*1000;
            read_ms=(s.storage_done-a)*1000; h2d_ms=submit_ms+sync_ms;
        } else {
            if(spdk) { message(begin_reads); REQUIRE(!s.error&&s.completed==COUNT); peak=s.peak; } else peak=uring_read(&ring,fd,qd);
            double b=stamp(CLOCK_MONOTONIC); c1=stamp(CLOCK_PROCESS_CPUTIME_ID);
            for(unsigned i=0;i<COUNT;i++)verify_record(i);
            double v=stamp(CLOCK_MONOTONIC); crc_ms=(v-b)*1000;
            if(gpu) {
                for(unsigned i=0;i<COUNT;i++) CUDA(cudaMemcpyAsync(device+(size_t)i*PAYLOAD,host+(size_t)i*STRIDE+HEADER,PAYLOAD,cudaMemcpyHostToDevice,stream));
                double u=stamp(CLOCK_MONOTONIC); submit_ms=(u-v)*1000;
                CUDA(cudaStreamSynchronize(stream)); sync_ms=(stamp(CLOCK_MONOTONIC)-u)*1000;
            }
            read_ms=(b-a)*1000; h2d_ms=(stamp(CLOCK_MONOTONIC)-v)*1000;
        }
        double d=stamp(CLOCK_MONOTONIC),c2=stamp(CLOCK_PROCESS_CPUTIME_ID);
        if(!s.pipeline)h2d_ms=(d-a)*1000-read_ms-crc_ms;
        if(spdk) { message(get_health); after=s.temp; } else after=kernel_temp(argv[2]);
        REQUIRE(peak==qd);
        REQUIRE(!memcmp(host,expected,(size_t)COUNT*STRIDE));
        if(gpu) {
            CUDA(cudaMemcpy(actual,device,(size_t)COUNT*PAYLOAD,cudaMemcpyDeviceToHost));
            for(unsigned i=0;i<COUNT;i++)REQUIRE(!memcmp(actual+(size_t)i*PAYLOAD,expected+(size_t)i*STRIDE+HEADER,PAYLOAD));
        }
        fprintf(out,"%s,%s,%s,%d,%u,%u,%u,32,32,16777216,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,16777216,%s,%d,%.6f,%.6f,%.6f\n",argv[1],argv[4],argv[8],(int)n-(int)warmup,n<warmup,qd,peak,read_ms,crc_ms,h2d_ms,(d-a)*1000,(c2-c0)*1000,(c1-c0)*1000,before,after,s.pipeline?"pipeline":"serial",s.pipeline,wait_ms,submit_ms,sync_ms);
        fflush(out);
    }
    fclose(out);
    if(spdk) { atomic_store(&s.stop,true); REQUIRE(!pthread_join(s.reactor,NULL)); }
    else {
        if(s.pipeline) { pthread_mutex_lock(&s.lock); atomic_store(&s.stop,true); pthread_cond_signal(&s.work_available); pthread_mutex_unlock(&s.lock); REQUIRE(!pthread_join(s.reactor,NULL)); }
        io_uring_queue_exit(&ring); close(fd);
    }
    CUDA(cudaFree(device)); CUDA(cudaStreamDestroy(stream)); CUDA(cudaHostUnregister(host));
    munlock(host,HOST_BYTES); munmap(host,HOST_BYTES); free(expected); free(actual);
    puts("all_rounds_identity_version_crc_cpu_gpu_bytes=PASS"); return 0;
}
