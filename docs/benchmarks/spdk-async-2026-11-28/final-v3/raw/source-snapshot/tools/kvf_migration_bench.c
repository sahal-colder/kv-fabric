/* Production aligned-record GPU/CPU <-> SSD migration, separate from raw I/O. */
#define _GNU_SOURCE
#include "miniinfer/miniinfer.h"
#include "miniinfer/device_transfer.h"
#include "../src/worker/io_metrics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do {mi_status s_=(x);if(s_!=MI_OK){fprintf(stderr,"%s: %s\n",#x,mi_status_string(s_));exit(1);}}while(0)
static unsigned char pattern(size_t n,unsigned block) { return (unsigned char)((n*61u)^(n>>8)^(block*17u)); }
static mi_storage_metrics metrics(mi_storage_backend *ssd,mi_storage_backend *memory) {
    mi_storage_metrics a,b;mi_backend_get_metrics(ssd,&a);mi_backend_get_metrics(memory,&b);
    return (mi_storage_metrics){a.io_ns+b.io_ns,a.crc_ns+b.crc_ns,a.host_copy_ns+b.host_copy_ns,
        a.d2h_ns+b.d2h_ns,a.h2d_ns+b.h2d_ns};
}
int main(int argc,char **argv) {
    if(argc!=13) {
        fprintf(stderr,"usage: %s spdk|uring TARGET BDEV OFFSET CAPACITY PAGE_BYTES PAGE_COUNT CONCURRENCY cpu|gpu ROUNDS WARMUP CSV\n",argv[0]);return 2;
    }
    int spdk=!strcmp(argv[1],"spdk"),device=!strcmp(argv[9],"gpu")?0:-1;
    if(!spdk&&strcmp(argv[1],"uring")) return 2;
    uint64_t offset=strtoull(argv[4],NULL,0);size_t capacity=strtoull(argv[5],NULL,0),bytes=strtoull(argv[6],NULL,0);
    unsigned count=(unsigned)strtoul(argv[7],NULL,0),concurrency=(unsigned)strtoul(argv[8],NULL,0);
    int rounds=atoi(argv[10]),warmup=atoi(argv[11]);
    if(!bytes||bytes%4096||!count||count>256||!concurrency||concurrency>128||rounds<1||warmup<0||
        count>capacity/(bytes+4096)) return 2;
    mi_location loc=device<0?MI_LOCATION_CPU:MI_LOCATION_GPU;
    mi_mempool *pool;mi_storage_backend *memory,*ssd;
    CHECK(mi_mempool_create(device<0?MI_MEMORY_CPU:MI_MEMORY_DEVICE,device,(mi_mempool_config){0},&pool));
    CHECK(mi_memory_backend_create(loc,bytes,pool,count,&memory));
    if(spdk) {
        mi_spdk_config cfg={.json_config=argv[2],.bdev_name=argv[3],.reactor_mask="0x4",.offset_bytes=offset,
            .capacity_bytes=capacity,.iova_mode="va",.base_virtaddr=0x1000000000ull};
        CHECK(mi_ssd_spdk_backend_create_ex(cfg,(mi_spdk_options){.queue_depth=concurrency,
            .buffer_count=concurrency,.submit_batch=concurrency,.max_transfer_bytes=1024*1024},bytes,MI_SSD_VERIFY_ON_READ,&ssd));
    } else CHECK(mi_ssd_raw_uring_backend_create(argv[2],offset,capacity,bytes,MI_SSD_VERIFY_ON_READ,concurrency,device,3,&ssd));
    mi_storage_config cfg={.block_bytes=bytes,.cpu_bytes=device<0?bytes*count:0,.gpu_bytes=device<0?0:bytes*count,
        .ssd_bytes=(bytes+4096)*count,.max_async_migrations=concurrency};
    mi_storage_backends backends={.cpu=device<0?memory:NULL,.gpu=device<0?NULL:memory,.ssd=ssd};
    mi_tiered_storage *storage;CHECK(mi_tiered_storage_create(cfg,backends,&storage));
    unsigned char *expected,*actual;CHECK(mi_device_host_alloc(device,bytes,(void **)&expected));
    CHECK(mi_device_host_alloc(device,bytes,(void **)&actual));
    mi_stream stream=NULL;if(device>=0) CHECK(mi_device_stream_create(device,&stream));
    mi_block_id ids[256];mi_status results[256];
    for(unsigned i=0;i<count;++i) {
        for(size_t j=0;j<bytes;++j) expected[j]=pattern(j,i);
        CHECK(mi_storage_create_block(storage,bytes,loc,&ids[i]));mi_lease lease;
        CHECK(mi_storage_acquire(storage,ids[i],MI_ACCESS_WRITE,stream,&lease));
        CHECK(mi_device_copy_from_host(device,lease.data,expected,bytes,stream));CHECK(mi_lease_finish(&lease));
    }
    FILE *out=fopen(argv[12],"w");if(!out) return 1;
    fputs("layer,backend,device,page_bytes,pages,concurrency,sample,direction,elapsed_ns,io_stage_ns,crc_stage_ns,host_copy_stage_ns,d2h_stage_ns,h2d_stage_ns,max_running,verified_bytes\n",out);
    for(int sample=-warmup;sample<rounds;++sample) {
        for(unsigned direction=0;direction<2;++direction) {
            mi_storage_metrics before=metrics(ssd,memory);uint64_t start=mi_io_now_ns();
            CHECK(mi_storage_migrate_batch(storage,ids,count,direction?loc:MI_LOCATION_SSD,results));
            uint64_t elapsed=mi_io_now_ns()-start;mi_storage_metrics after=metrics(ssd,memory);
            mi_migration_stats stats;mi_storage_migration_stats(storage,&stats);
            fprintf(out,"B-migration,%s,%s,%zu,%u,%u,%d,%s,%llu,%llu,%llu,%llu,%llu,%llu,%zu,%zu\n",
                argv[1],argv[9],bytes,count,concurrency,sample,direction?"recovery":"spill",(unsigned long long)elapsed,
                (unsigned long long)(after.io_ns-before.io_ns),(unsigned long long)(after.crc_ns-before.crc_ns),
                (unsigned long long)(after.host_copy_ns-before.host_copy_ns),(unsigned long long)(after.d2h_ns-before.d2h_ns),
                (unsigned long long)(after.h2d_ns-before.h2d_ns),stats.max_running,direction?bytes*count:0);
        }
        /* Strict byte/version check is outside both timed intervals. ON_READ CRC remains inside recovery. */
        for(unsigned i=0;i<count;++i) {
            mi_block_meta meta;CHECK(mi_storage_read_snapshot(storage,ids[i],1,actual,bytes,stream,&meta));
            for(size_t j=0;j<bytes;++j) if(actual[j]!=pattern(j,i)) {fputs("migration byte mismatch\n",stderr);return 1;}
        }
        fflush(out);
    }
    fclose(out);if(stream) CHECK(mi_device_stream_destroy(device,stream));
    CHECK(mi_device_host_free(device,actual));CHECK(mi_device_host_free(device,expected));
    mi_tiered_storage_destroy(storage);mi_backend_destroy(ssd);mi_backend_destroy(memory);mi_mempool_destroy(pool);
    return 0;
}
