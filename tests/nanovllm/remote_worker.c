/* An independent Worker process driven through the Nano-VLLM scheduling API. */
#define _POSIX_C_SOURCE 200809L
#include "nano_cache.h"
#include "kvfabric/kvfabric.h"
#ifdef NANO_WITH_CUDA
#include <cuda_runtime_api.h>
#endif
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        nv_fatal("test clock");
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void pause_ms(unsigned ms) {
    struct timespec pause = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    (void)nanosleep(&pause, NULL);
}

static uint64_t cpu_ns(struct rusage r) {
    return ((uint64_t)r.ru_utime.tv_sec + (uint64_t)r.ru_stime.tv_sec) * UINT64_C(1000000000) +
           ((uint64_t)r.ru_utime.tv_usec + (uint64_t)r.ru_stime.tv_usec) * 1000;
}

static size_t gpu_free_bytes(void) {
#ifdef NANO_WITH_CUDA
    size_t available, total;
    if (cudaMemGetInfo(&available, &total) == cudaSuccess)
        return available;
#endif
    return 0;
}

static void print_page_hashes(NvCache *cache, const NvSequence *s) {
    size_t half = cache->elements / (size_t)cache->num_blocks * sizeof(float);
    void *page = nv_alloc(2, half);
    fputs(",\"kv_page_hashes\":[", stdout);
    /* One finished request, with no subsequent forward that could reuse a slot. */
    for (int i = 0; i < s->prompt_len / cache->block_size; ++i) {
        nv_cache_copy_block(cache, s->block_table[i], page, half, 0);
        kvf_hash hash;
        char hex[65];
        if (kvf_hash_bytes(page, 2 * half, &hash) != KVF_OK)
            nv_fatal("test page hash failed");
        kvf_hash_format(hash, hex);
        printf("%s\"%s\"", i ? "," : "", hex);
    }
    fputc(']', stdout);
    free(page);
}

static void request(NvEngine *e, NvTokenizer *t, int max_new, size_t gpu_free_initial,
                    char *command, char *state) {
    const int cancel = strcmp(command, "cancel") == 0;
    const int sync = strcmp(command, "sync") == 0;
    const int settle = strcmp(command, "settle") == 0;
    const int stall = strcmp(command, "stall") == 0;
    const int inspect = strcmp(command, "inspect") == 0;
    const int overlap = strcmp(command, "overlap") == 0;
    if (!cancel && !sync && !settle && !stall && !inspect && !overlap && strcmp(command, "infer") != 0)
        nv_fatal("unknown test command");

    int first = e->num_requests;
    uint64_t local = e->cache.hits, prefilled = e->prefill_tokens, forwards = e->forward_tokens;
    NvRemoteStats before, after;
    nv_engine_remote_stats(e, &before);
    struct rusage usage_before = {0}, usage_after = {0};
    (void)getrusage(RUSAGE_SELF, &usage_before);
    uint64_t start = now_ns();
    for (char *prompt = strtok_r(NULL, "\t\r\n", &state); prompt;
         prompt = strtok_r(NULL, "\t\r\n", &state)) {
        int count;
        int *tokens = nv_encode(t, prompt, &count);
        NvSampling sampling = {.temperature = 0.7f, .top_p = 0.95f, .seed = 1234, .ignore_eos = 1};
        int id = nv_engine_add(e, tokens, count, max_new, sampling);
        free(tokens);
        if (id < 0)
            nv_fatal("test request exceeds context, page pool, or request count");
    }
    int count = e->num_requests - first;
    if (count < 1 || ((cancel || inspect) && count != 1))
        nv_fatal("invalid test request count");

    if (overlap && (count != 2 || nv_engine_step(e) == 0 ||
        e->requests[first]->status != NV_REMOTE_PREFETCHING ||
        e->requests[first + 1]->num_tokens <= e->requests[first + 1]->prompt_len))
        nv_fatal("a ready request must produce a token while another waits for remote KV");

    if (cancel || settle || stall) {
        if (nv_engine_step(e) != 0 || e->requests[first]->status != NV_REMOTE_PREFETCHING)
            nv_fatal("expected an asynchronously prefetching request");
        pause_ms(cancel ? 20 : (stall ? 650 : 200));
        if (cancel && !nv_engine_cancel(e, first))
            nv_fatal("prefetch cancellation failed");
    }

    NvSequence *lead = e->requests[first];
    uint64_t first_token = 0;
    while (nv_engine_has_work(e)) {
        int used = nv_engine_step(e);
        if (!first_token && lead->num_tokens > lead->prompt_len)
            first_token = now_ns();
        if (sync)
            nv_engine_remote_flush(e);
        if (!used)
            pause_ms(1);
    }
    uint64_t end = now_ns();
    (void)getrusage(RUSAGE_SELF, &usage_after);
    nv_engine_remote_flush(e);
    nv_engine_remote_stats(e, &after);

    printf("{\"tokens\":[");
    for (int i = first; i < e->num_requests; ++i) {
        NvSequence *s = e->requests[i];
        printf("%s[", i == first ? "" : ",");
        for (int j = s->prompt_len; j < s->num_tokens; ++j)
            printf("%s%d", j == s->prompt_len ? "" : ",", s->tokens[j]);
        fputc(']', stdout);
    }
    printf("],\"prompt_tokens\":%d,\"prefilled\":%" PRIu64
           ",\"forwards\":%" PRIu64 ",\"local\":%" PRIu64
           ",\"ttft_ns\":%" PRIu64 ",\"prefill_ns\":%" PRIu64 ",\"decode_ns\":%" PRIu64
           ",\"cpu_ns\":%" PRIu64 ",\"rss_kib\":%ld",
           lead->prompt_len, e->prefill_tokens - prefilled, e->forward_tokens - forwards, e->cache.hits - local,
           first_token ? first_token - start : 0, lead->prefill_done_ns ? lead->prefill_done_ns - start : 0,
           first_token ? end - first_token : 0, cpu_ns(usage_after) - cpu_ns(usage_before), usage_after.ru_maxrss);
    size_t gpu_free = gpu_free_initial ? gpu_free_bytes() : 0;
    if (gpu_free && gpu_free <= gpu_free_initial)
        printf(",\"gpu_device_delta_bytes\":%zu", gpu_free_initial - gpu_free);
    else
        fputs(",\"gpu_device_delta_bytes\":null", stdout);
    if (inspect)
        print_page_hashes(&e->cache, lead);
#define REPORT(field) printf(",\"" #field "\":%" PRIu64, after.field - before.field)
    REPORT(remote_lookup_requests);
    REPORT(remote_lookup_hits);
    REPORT(remote_hit_pages);
    REPORT(remote_hit_tokens);
    REPORT(remote_import_bytes);
    REPORT(remote_prefetch_requests);
    REPORT(remote_prefetch_failures);
    REPORT(remote_prefetch_cancelled);
    REPORT(remote_prefill_skipped_tokens);
    REPORT(remote_prefill_fallbacks);
    REPORT(remote_cache_evictions);
    REPORT(remote_transfer_latency_ns);
    REPORT(remote_transfer_samples);
#undef REPORT
    printf(",\"remote_published_pages\":%" PRIu32, after.remote_published_pages);
    size_t sample_count;
    const uint64_t *samples = nv_engine_remote_take_transfer_samples(e, &sample_count);
    if (sample_count != after.remote_transfer_samples - before.remote_transfer_samples)
        nv_fatal("incomplete page transfer trace");
    printf(",\"page_transfer_ns\":[");
    for (size_t i = 0; i < sample_count; ++i)
        printf("%s%" PRIu64, i ? "," : "", samples[i]);
    puts("]}");
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc != 15)
        nv_fatal("test worker expects MODEL TOKENIZER DEVICE MODE CONTROL LISTEN ADVERTISE NAME TENANT PAGE BLOCKS CAPACITY TIMEOUT NEW");
    NvOptions options = {.block_size = atoi(argv[10]), .num_blocks = atoi(argv[11]),
        .max_sequences = 4, .token_budget = 16, .max_requests = 4096,
        .prefix_cache = strcmp(argv[4], "cold") != 0};
    const int use_cuda = strcmp(argv[3], "cuda") == 0;
#ifdef NANO_WITH_CUDA
    if (use_cuda) {
        int devices = 0;
        if (cudaGetDeviceCount(&devices) != cudaSuccess || !devices)
            return 77;
    }
#else
    if (use_cuda)
        return 77;
#endif
    size_t gpu_free_initial = use_cuda ? gpu_free_bytes() : 0;
    NvModel model;
    NvTokenizer tokenizer;
    NvEngine engine;
    nv_model_load(&model, argv[1]);
    nv_tokenizer_load(&tokenizer, argv[2], model.config.vocab_size);
    if (!use_cuda)
        nv_engine_init(&engine, &model, options);
#ifdef NANO_WITH_CUDA
    else
        nv_engine_init_cuda(&engine, &model, options, 0);
#endif
    if (strcmp(argv[4], "remote") == 0) {
        NvRemoteOptions remote;
        nv_remote_options_defaults(&remote);
        remote.control_endpoint = argv[5];
        remote.listen_endpoint = argv[6];
        remote.advertise_endpoint = strcmp(argv[7], "-") ? argv[7] : NULL;
        remote.worker_name = argv[8];
        remote.tenant = argv[9];
        remote.cache_capacity = (unsigned)atoi(argv[12]);
        remote.prefetch_timeout_ms = (unsigned)atoi(argv[13]);
        remote.transfer_sample_capacity = (size_t)options.max_sequences * (size_t)options.num_blocks;
        if (nv_engine_enable_remote(&engine, &tokenizer, remote) != 1)
            nv_fatal("could not start Nano-VLLM KV worker");
    }

    puts("READY");
    fflush(stdout);
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, stdin) >= 0) {
        char *state;
        char *command = strtok_r(line, "\t\r\n", &state);
        if (command)
            request(&engine, &tokenizer, atoi(argv[14]), gpu_free_initial, command, state);
    }
    free(line);
    nv_engine_free(&engine);
    nv_tokenizer_free(&tokenizer);
    nv_model_free(&model);
    return 0;
}
