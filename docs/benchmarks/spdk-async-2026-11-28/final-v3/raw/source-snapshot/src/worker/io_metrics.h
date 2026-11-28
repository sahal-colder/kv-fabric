#ifndef MINIINFER_IO_METRICS_H
#define MINIINFER_IO_METRICS_H
#include <stdint.h>
#include <time.h>
typedef struct mi_io_metrics {
    uint64_t queued_ns, started_ns, completed_ns, copy_ns;
} mi_io_metrics;
static inline uint64_t mi_io_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
}
#endif
