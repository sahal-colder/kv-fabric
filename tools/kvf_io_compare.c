/* Controlled file-I/O experiment, not LLM TTFT or the production file format.
 * Equal 4KiB header + 16KiB payload records and CRC for all three APIs.
 * Each lane has one outstanding operation, matching the production Ring API.
 */
#define _GNU_SOURCE
#include "kvfabric/kvfabric.h"
#include "io_transfer.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { PAYLOAD = 16384, STRIDE = 20480, RECORDS = 8192, PER_REQUEST = 512 };
typedef struct {
    const char *mode;
    int fd, requests, lanes;
    atomic_int next, failed;
    pthread_barrier_t ready;
    uint32_t crc[RECORDS];
    double *ms;
} experiment;
typedef struct { experiment *e; int index; } lane;

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static void die(const char *s) { perror(s); exit(1); }
static void *run(void *arg) {
    lane *l = arg;
    experiment *e = l->e;
    void *allocation = NULL, *buffer;
    mi_uring *ring = NULL;
    if (!strcmp(e->mode, "fixed")) {
        if (mi_uring_create(e->fd, 2, &ring) != MI_OK ||
            mi_uring_register_buffer(ring, -1, STRIDE)) die("fixed buffer setup");
        buffer = mi_uring_buffer(ring);
    } else {
        if (posix_memalign(&allocation, 4096, STRIDE)) die("aligned buffer");
        buffer = allocation;
    }
    pthread_barrier_wait(&e->ready);
    pthread_barrier_wait(&e->ready);
    for (;;) {
        int request = atomic_fetch_add(&e->next, 1);
        if (request >= e->requests) break;
        double start = now();
        for (unsigned j = 0; j < PER_REQUEST; ++j) {
            /* Same deterministic access order for every API, full-file coverage. */
            unsigned record = ((unsigned)request * PER_REQUEST + j) * 4051u % RECORDS;
            uint64_t offset = (uint64_t)record * STRIDE;
            int ok;
            if (ring) {
                mi_io_range range = {.data=buffer, .data_bytes=STRIDE, .offset=offset};
                ok = mi_uring_transfer_fixed(ring, 0, range) == MI_OK;
            } else {
                ssize_t n;
                do { n = pread(e->fd, buffer, STRIDE, (off_t)offset); } while (n < 0 && errno == EINTR);
                ok = n == STRIDE;
            }
            uint32_t id;
            memcpy(&id, buffer, sizeof(id));
            if (!ok || id != record || kvf_crc32c((char *)buffer + 4096, PAYLOAD) != e->crc[record]) {
                atomic_store(&e->failed, 1);
                break;
            }
        }
        e->ms[request] = (now() - start) * 1000;
    }
    mi_uring_destroy(ring);
    free(allocation);
    return NULL;
}
static size_t resident(int fd, size_t bytes) {
    void *map = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) die("mmap");
    size_t pages = (bytes + 4095) / 4096, count = 0;
    unsigned char *vec = malloc(pages);
    if (!vec || mincore(map, bytes, vec)) die("mincore");
    for (size_t i = 0; i < pages; ++i) count += (vec[i] & 1) != 0;
    free(vec);
    munmap(map, bytes);
    return count * 4096;
}
static long rss(void) {
    long total, pages;
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f || fscanf(f, "%ld %ld", &total, &pages) != 2) die("statm");
    fclose(f);
    return pages * sysconf(_SC_PAGESIZE);
}
int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s PRIVATE_DIR {fixed|direct|buffered} LANES REQUESTS\n", argv[0]); return 2; }
    experiment e = {.mode=argv[2], .lanes=atoi(argv[3]), .requests=atoi(argv[4])};
    if (e.lanes < 1 || e.lanes > 64 || e.requests < 100 || e.requests > 10000 ||
        (strcmp(e.mode,"fixed") && strcmp(e.mode,"direct") && strcmp(e.mode,"buffered"))) return 2;
    char path[4096];
    if (snprintf(path,sizeof(path),"%s/io-compare.XXXXXX",argv[1]) >= (int)sizeof(path)) return 2;
    int fd = mkstemp(path);
    if (fd < 0) die("mkstemp");
    unsigned char *data = malloc(STRIDE);
    if (!data) die("malloc");
    for (uint32_t i=0; i<RECORDS; ++i) {
        memset(data,0,4096);
        memcpy(data,&i,sizeof(i));
        for (size_t j=4096;j<STRIDE;++j) data[j]=(unsigned char)(j*61u+i*17u);
        e.crc[i]=kvf_crc32c(data+4096,PAYLOAD);
        if (write(fd,data,STRIDE)!=STRIDE) die("write");
    }
    free(data);
    if (fsync(fd)) die("fsync");
    if (posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED)) die("fadvise");
    size_t file_bytes=(size_t)RECORDS*STRIDE;
    size_t cache_before=resident(fd,file_bytes);
    close(fd);
    e.fd=open(path,O_RDONLY|O_CLOEXEC|(!strcmp(e.mode,"buffered")?0:O_DIRECT));
    if (e.fd<0) die("open");
    unlink(path);
    atomic_init(&e.next,0); atomic_init(&e.failed,0);
    e.ms=calloc((size_t)e.requests,sizeof(double));
    pthread_barrier_init(&e.ready,NULL,(unsigned)e.lanes+1);
    pthread_t threads[64]; lane lanes[64];
    for (int i=0;i<e.lanes;++i) {
        lanes[i]=(lane){&e,i};
        if (pthread_create(&threads[i],NULL,run,&lanes[i])) die("pthread_create");
    }
    pthread_barrier_wait(&e.ready); /* All lane buffers are registered and ready. */
    long rss_before=rss();
    struct rusage before,after;
    getrusage(RUSAGE_SELF,&before);
    double start=now();
    pthread_barrier_wait(&e.ready);
    for (int i=0;i<e.lanes;++i) pthread_join(threads[i],NULL);
    double wall=now()-start;
    getrusage(RUSAGE_SELF,&after);
    size_t cache_after=resident(e.fd,file_bytes);
    long rss_after=rss();
    puts("mode,lanes,request,payload_bytes,latency_ms");
    for (int i=0;i<e.requests;++i) printf("%s,%d,%d,%d,%.6f\n",e.mode,e.lanes,i,PER_REQUEST*PAYLOAD,e.ms[i]);
    double sys=(after.ru_stime.tv_sec-before.ru_stime.tv_sec)+(after.ru_stime.tv_usec-before.ru_stime.tv_usec)*1e-6;
    double user=(after.ru_utime.tv_sec-before.ru_utime.tv_sec)+(after.ru_utime.tv_usec-before.ru_utime.tv_usec)*1e-6;
    fprintf(stderr,"{\"mode\":\"%s\",\"lanes\":%d,\"requests\":%d,\"wall_s\":%.6f,\"cpu_sys_s\":%.6f,\"cpu_user_s\":%.6f,\"file_bytes\":%zu,\"file_cached_before_bytes\":%zu,\"file_cached_after_bytes\":%zu,\"rss_before_bytes\":%ld,\"rss_after_bytes\":%ld,\"maxrss_KiB\":%ld,\"verified\":%s}\n",e.mode,e.lanes,e.requests,wall,sys,user,file_bytes,cache_before,cache_after,rss_before,rss_after,after.ru_maxrss,atomic_load(&e.failed)?"false":"true");
    close(e.fd); pthread_barrier_destroy(&e.ready); free(e.ms);
    return atomic_load(&e.failed)?1:0;
}
