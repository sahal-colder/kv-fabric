/* Production SSD engines only. Physical writes require the external safety runner. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "../src/worker/spdk_bridge.h"
#include "../src/worker/storage_crc.h"
#include <errno.h>
#include <dirent.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do {mi_status s_=(x);if(s_!=MI_OK){fprintf(stderr,"%s: %s\n",#x,mi_status_string(s_));exit(1);}}while(0)
static uint64_t number(const char *s) {
    char *end;errno=0;unsigned long long n=strtoull(s,&end,0);
    if(errno||!*s||*s=='-'||*end) exit(2);
    return n;
}
static double cpu_now(void) {
    struct timespec t;clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t);return t.tv_sec+t.tv_nsec*1e-9;
}
static uint64_t random_next(uint64_t *x) { *x^=*x<<13;*x^=*x>>7;*x^=*x<<17;return *x; }
static unsigned char pattern(size_t i) { return (unsigned char)((i*61u)^(i>>8)); }
static int compare_double(const void *a,const void *b) { double x=*(const double *)a,y=*(const double *)b;return (x>y)-(x<y); }
static double quantile(double *v,size_t n,double p) { return v[(size_t)((n-1)*p)]; }
/* One hardware counter per existing process thread; no wall-clock TSC proxy.
 * Enable/disable control is outside throughput timing. Values include the short
 * counter-control boundary and are scaled when PMU scheduling multiplexes. */
typedef struct cycle_counters { int fds[256]; unsigned count; int available; } cycle_counters;
static cycle_counters cycles_open(void) {
    cycle_counters c={0}; DIR *dir=opendir("/proc/self/task");
    if(!dir) return c;
    struct dirent *entry;
    while((entry=readdir(dir))) {
        if(entry->d_name[0]=='.') continue;
        if(c.count==256) break;
        struct perf_event_attr attr={.type=PERF_TYPE_HARDWARE,.size=sizeof(attr),
            .config=PERF_COUNT_HW_CPU_CYCLES,.disabled=1,
            .read_format=PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING};
        int fd=(int)syscall(__NR_perf_event_open,&attr,atoi(entry->d_name),-1,-1,0);
        if(fd<0) { while(c.count) close(c.fds[--c.count]); closedir(dir); return c; }
        c.fds[c.count++]=fd;
    }
    closedir(dir); c.available=c.count>0; return c;
}
static void cycles_start(cycle_counters *c) {
    for(unsigned i=0;i<c->count;++i) {
        if(ioctl(c->fds[i],PERF_EVENT_IOC_RESET,0)||ioctl(c->fds[i],PERF_EVENT_IOC_ENABLE,0)) c->available=0;
    }
}
static double cycles_stop(cycle_counters *c) {
    double total=0;
    for(unsigned i=0;i<c->count;++i) {
        if(ioctl(c->fds[i],PERF_EVENT_IOC_DISABLE,0)) c->available=0;
        struct {uint64_t count,enabled,running;} value={0};
        if(read(c->fds[i],&value,sizeof(value))!=sizeof(value)) c->available=0;
        if(value.count && !value.running) c->available=0;
        if(value.running) total+=(double)value.count*value.enabled/value.running;
    }
    return total;
}
int main(int argc,char **argv) {
    if(argc<13) {
        fprintf(stderr,"usage: %s raw BACKEND TARGET BDEV OFFSET CAPACITY BYTES QD WORKLOAD ROUNDS WARMUP OUTPUT_PREFIX [write-authorized] [iopoll] [sqpoll] [malloc]\n",argv[0]);return 2;
    }
    int spdk=!strcmp(argv[2],"spdk"),uring=!strcmp(argv[2],"uring");
    if(strcmp(argv[1],"raw")||(!spdk&&!uring)) return 2;
    uint64_t offset=number(argv[5]);size_t capacity=number(argv[6]),bytes=number(argv[7]);
    unsigned qd=(unsigned)number(argv[8]),rounds=(unsigned)number(argv[10]),warmup=(unsigned)number(argv[11]);
    const char *work=argv[9];int sequential=!strncmp(work,"seq",3),mixed=!strcmp(work,"mixed");
    int write=!strcmp(work,"seq-write")||!strcmp(work,"rand-write");
    if(!sequential&&!mixed&&strcmp(work,"rand-read")&&strcmp(work,"rand-write")) return 2;
    if(sequential&&strcmp(work,"seq-read")&&strcmp(work,"seq-write")) return 2;
    int authorized=0,iopoll=0,sqpoll=0,malloc_mode=0;
    for(int i=13;i<argc;++i) {
        if(!strcmp(argv[i],"write-authorized")) authorized=1;
        else if(!strcmp(argv[i],"iopoll")) iopoll=1;
        else if(!strcmp(argv[i],"sqpoll")) sqpoll=1;
        else if(!strcmp(argv[i],"malloc")) malloc_mode=1;
        else return 2;
    }
    if((write||mixed)&&!authorized) { fputs("writes require backup-verified runner authorization\n",stderr);return 2; }
    if(!bytes||bytes%4096||bytes>4*1024*1024||!qd||qd>128||capacity<bytes||capacity%4096||offset%4096||
        offset>INT64_MAX||capacity>INT64_MAX-offset||rounds<1||rounds>1000||warmup>100) return 2;
    unsigned operations=(unsigned)((16*1024*1024)/bytes);
    if(operations>512) operations=512;
    if(operations<qd) operations=qd;
    operations=(operations+qd-1)/qd*qd;
    char path[4096];snprintf(path,sizeof(path),"%s-rounds.csv",argv[12]);FILE *summary=fopen(path,"w");
    snprintf(path,sizeof(path),"%s-io.csv",argv[12]);FILE *raw=fopen(path,"w");
    if(!summary||!raw) return 1;
    /* Open counters before creating any engine threads; permission failure is recorded by runner perf stat. */
    mi_spdk_device *sd=NULL;mi_uring *ud=NULL;int fd=-1;
    if(spdk) {
        mi_spdk_config cfg={.json_config=argv[3],.bdev_name=argv[4],.reactor_mask="0x4",
            .offset_bytes=offset,.capacity_bytes=capacity,.iova_mode="va",.base_virtaddr=0x1000000000ull,
            .no_huge=malloc_mode,.no_pci=malloc_mode};
        CHECK(mi_spdk_open_ex(cfg,(mi_spdk_options){.queue_depth=qd,.buffer_count=qd,.submit_batch=qd,
                    .max_transfer_bytes=bytes},&sd));
    } else {
        /* Exclusive block opens may briefly remain held by deferred kernel fput
         * or udev after the preceding process/driver switch. Never force ownership. */
        for(unsigned retry=0;retry<200;++retry) {
            fd=open(argv[3],O_RDWR|O_DIRECT|O_CLOEXEC|O_EXCL);
            if(fd>=0 || errno!=EBUSY) break;
            usleep(10000);
        }
        if(fd<0){perror("open");return 1;}
        CHECK(mi_uring_create_ex(fd,(mi_uring_options){.queue_depth=qd,.submit_batch=qd,.buffer_bytes=bytes,
            .fixed_buffers=1,.fixed_file=1,.iopoll=iopoll,.sqpoll=sqpoll,.worker_cpu_plus_one=3},&ud));
    }
    unsigned char *buffers; if(posix_memalign((void **)&buffers,4096,bytes*qd)) return 1;
    unsigned char *expected=malloc(bytes);double *latencies=malloc(operations*sizeof(*latencies));
    if(!expected||!latencies) return 1;
    for(size_t i=0;i<bytes;++i) expected[i]=pattern(i%4096);
    uint32_t crc=mi_crc32c(0,expected,bytes);
    if(uring) CHECK(mi_uring_transfer(ud,0,(mi_io_range){0})); /* start owner before PMU enumeration */
    cycle_counters counters=cycles_open();
    fprintf(summary,"backend,workload,bytes,qd,sample,operations,seconds,cpu_seconds,iops,mib_s,p50_us,p95_us,p99_us,p999_us,queue_wait_ns,copy_ns,crc_ns,verified_bytes,max_inflight,submitted,completed,fixed_buffers,fixed_file,iopoll,sqpoll,cpu_cycles,cycles_measured\n");
    fprintf(raw,"backend,workload,bytes,qd,sample,operation,write,offset,queued_ns,started_ns,completed_ns,copy_ns\n");
    for(int sample=-(int)warmup;sample<(int)rounds;++sample) {
        double elapsed=0,cpu=0,cycles=0;uint64_t queue_wait=0,copy=0,crc_ns=0,verified=0;
        uint64_t seed=0x9e3779b97f4a7c15ull+(uint64_t)(sample+(int)warmup);
        for(unsigned base=0;base<operations;base+=qd) {
            mi_spdk_request *sr[128]={0};mi_uring_request *ur[128]={0};
            mi_io_metrics metrics[128];uint64_t addresses[128];int writes[128];
            for(unsigned j=0;j<qd;++j) {
                writes[j]=mixed ? random_next(&seed)%100>=70 : write;
                uint64_t index=sequential ? (base+j)%(capacity/bytes) : random_next(&seed)%(capacity/bytes);
                addresses[j]=index*bytes;
                if(writes[j]) memcpy(buffers+j*bytes,expected,bytes);
            }
            cycles_start(&counters);
            double c=cpu_now();uint64_t t=mi_io_now_ns();
            for(unsigned j=0;j<qd;++j) {
                mi_io_range r={.data=buffers+j*bytes,.data_bytes=bytes,.offset=addresses[j]+(spdk?0:offset)};
                if(spdk) CHECK(mi_spdk_submit(sd,writes[j],r,&sr[j]));
                else CHECK(mi_uring_submit(ud,writes[j],r,&ur[j]));
            }
            for(unsigned j=0;j<qd;++j) {
                if(spdk) CHECK(mi_spdk_wait_metrics(sr[j],&metrics[j]));
                else CHECK(mi_uring_wait_metrics(ur[j],&metrics[j]));
            }
            elapsed+=(mi_io_now_ns()-t)*1e-9;cpu+=cpu_now()-c;
            cycles+=cycles_stop(&counters);
            uint64_t v=mi_io_now_ns();
            for(unsigned j=0;j<qd;++j) {
                if(!writes[j]) {
                    if(mi_crc32c(0,buffers+j*bytes,bytes)!=crc || memcmp(buffers+j*bytes,expected,bytes)) {
                        fputs("payload CRC/byte mismatch\n",stderr);return 1;
                    }
                    verified+=bytes;
                }
            }
            crc_ns+=mi_io_now_ns()-v;
            for(unsigned j=0;j<qd;++j) {
                mi_io_metrics m=metrics[j];latencies[base+j]=(m.completed_ns-m.queued_ns)*1e-3;
                queue_wait+=m.started_ns-m.queued_ns;copy+=m.copy_ns;
                fprintf(raw,"%s,%s,%zu,%u,%d,%u,%d,%llu,%llu,%llu,%llu,%llu\n",argv[2],work,bytes,qd,sample,base+j,writes[j],
                    (unsigned long long)(offset+addresses[j]),(unsigned long long)m.queued_ns,(unsigned long long)m.started_ns,
                    (unsigned long long)m.completed_ns,(unsigned long long)m.copy_ns);
            }
        }
        unsigned high,inflight;uint64_t submitted,completed;int fixed_buffers=0,fixed_file=0,ip=0,sp=0;
        if(spdk) { mi_spdk_stats s;mi_spdk_get_stats(sd,&s);high=s.max_inflight;inflight=s.inflight;submitted=s.submitted;completed=s.completed; }
        else { mi_uring_stats s;mi_uring_get_stats(ud,&s);high=s.max_inflight;inflight=s.inflight;submitted=s.submitted;completed=s.completed;
            fixed_buffers=s.fixed_buffers;fixed_file=s.fixed_file;ip=s.iopoll;sp=s.sqpoll; }
        if(inflight||submitted!=completed) return 1;
        qsort(latencies,operations,sizeof(*latencies),compare_double);
        fprintf(summary,"%s,%s,%zu,%u,%d,%u,%.9f,%.9f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%llu,%llu,%u,%llu,%llu,%d,%d,%d,%d,%.0f,%d\n",
            argv[2],work,bytes,qd,sample,operations,elapsed,cpu,operations/elapsed,operations*bytes/elapsed/1048576.,
            quantile(latencies,operations,.5),quantile(latencies,operations,.95),quantile(latencies,operations,.99),quantile(latencies,operations,.999),
            (unsigned long long)queue_wait,(unsigned long long)copy,(unsigned long long)crc_ns,(unsigned long long)verified,high,
            (unsigned long long)submitted,(unsigned long long)completed,fixed_buffers,fixed_file,ip,sp,cycles,counters.available);
        fflush(summary);fflush(raw);
    }
    for(unsigned i=0;i<counters.count;++i) close(counters.fds[i]);
    if(spdk) mi_spdk_close(sd);else { mi_uring_destroy(ud);close(fd); }
    fclose(raw);fclose(summary);free(expected);free(buffers);free(latencies);return 0;
}
