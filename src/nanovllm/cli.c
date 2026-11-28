/*
 * 单机推理的参数、交互循环和运行时资源装配。
 * CLI 拥有模型、词表、引擎和可选本地 SSD。命令循环单线程驱动。
 */
#define _POSIX_C_SOURCE 200809L
#include "nano_cache.h"
#include "nano_cli.h"
#ifdef NANO_WITH_REMOTE_KV
#include "miniinfer/remote_cache.h"
#endif
#ifdef NANO_WITH_STORAGE
#include "miniinfer/model_mapping.h"
#include "miniinfer/storage_backend.h"
#endif
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NV_DEFAULT_TOKENIZER_PATH "models/tokenizer.bin"
#define NV_DEFAULT_PROMPT "Once upon a time"
#define NV_DEFAULT_MAX_NEW_TOKENS 64
#define NV_DEFAULT_TEMPERATURE 0.8f
#define NV_DEFAULT_TOP_P 0.9f
#define NV_DEFAULT_SEED UINT64_C(1)
#define NV_DEFAULT_KV_BLOCKS 128
#define NV_DEFAULT_KV_BLOCK_SIZE 16
#define NV_DEFAULT_MAX_SEQUENCES 4
#define NV_DEFAULT_TOKEN_BUDGET 32
#define NV_DEFAULT_PREFIX_CACHE 1
#define NV_DEFAULT_TRACE 0
#define NV_DEFAULT_IDS 0
#define NV_DEFAULT_IGNORE_EOS 0

#ifdef NANO_WITH_STORAGE
static const char cli_name[] = "miniinfer";
/* 从集成 CLI 参数解析出的 SSD 后端配置。 */
typedef struct {
    const char *ssd_mode;
    const char *ssd_dir;
    int ssd_slots;
    int spdk_offset_set;
    mi_spdk_config spdk;
} StorageOptions;

#else
static const char cli_name[] = "nanovllm";
#endif

static void usage(FILE *file) {
    fprintf(file,
        "Usage: %s MODEL [options]\n"
        "  -z PATH       tokenizer (default models/tokenizer.bin)\n"
        "  -i TEXT       prompt; repeat for continuous batching\n"
        "  --stdin       read one prompt per line, keeping the KV cache until EOF\n"
        "  -n N          max NEW tokens per request (default 64)\n"
        "  -t F          temperature; 0 = greedy (default 0.8)\n"
        "  -p F          top-p in (0,1] (default 0.9)\n"
        "  -s N          random seed, positive (default 1)\n"
        "  --device cpu|cuda (default cpu)\n"
        "  --blocks N    KV pool blocks (default 128)\n"
        "  --block-size N tokens per block (default 16)\n"
        "  --batch N     maximum active requests (default 4)\n"
        "  --budget N    forward tokens per scheduling tick (default 32)\n"
        "  --no-cache    disable prefix reuse\n"
        "  --trace       print scheduling progress on stderr\n"
        "  --ids         print generated token IDs instead of text\n"
        "  --ignore-eos  generate exactly N tokens\n", cli_name);
#ifdef NANO_WITH_REMOTE_KV
    fputs(
        "  --remote-kv                enable cross-worker KV prefix reuse\n"
        "  --kv-control-endpoint IP:PORT    directory (default 127.0.0.1:9000)\n"
        "  --kv-listen-endpoint IP:PORT     worker bind (default 127.0.0.1:0)\n"
        "  --kv-advertise-endpoint IP:PORT  reachable worker address\n"
        "  --kv-transport tcp|rdma  worker data transport (default tcp)\n"
        "  --[no-]kv-rdma-fallback  allow TCP fallback when RDMA is selected (default on)\n"
        "  --kv-tenant NAME           cache namespace (default default)\n"
        "  --kv-page-size N           tokens per page, same as --block-size\n"
        "  --kv-cache-capacity N      worker export pages (default --blocks)\n"
        "  --kv-prefetch-timeout MS   timeout in milliseconds (default 1000)\n"
        "  --kv-publish-generated    export complete pages of forwarded generated tokens\n"
        "  --kv-worker-name NAME      override the worker registry name\n"
        "  --kv-tp-size N --kv-pp-size N    Nano-VLLM supports 1 for both\n", file);
#endif
#ifdef NANO_WITH_STORAGE
    fputs(
            "  --ssd-slots N number of spill slots (default 0, disabled)\n"
            "  --ssd-mode posix|uring|spdk (default posix)\n"
            "  --ssd-dir DIR directory for the private POSIX/io_uring spill file\n"
            "  --spdk-config PATH --spdk-bdev NAME --spdk-offset BYTES\n"
            "                explicit bdev and dedicated byte region\n"
            "  --spdk-no-pci use the SPDK software bdev environment\n"
            "  --spdk-reactor-mask MASK (default 0x1)\n"
            "  --spdk-iova-mode va|pa (SDK default if omitted)\n"
            "  --spdk-base-virtaddr BYTES (accepts 0x hexadecimal)\n", file);
#endif
}

static int integer(const char *s) {
    char *end;
    errno = 0;
    long n = strtol(s, &end, 10);
    if (errno || !*s || *end || n < 1 || n > INT_MAX) nv_fatal("expected positive integer option");
    return (int)n;
}

#ifdef NANO_WITH_REMOTE_KV
static int parse_remote(NvRemoteOptions *o, const char *flag, const char *value) {
    if (strcmp(flag, "--kv-control-endpoint") == 0)
        o->control_endpoint = value;
    else if (strcmp(flag, "--kv-listen-endpoint") == 0)
        o->listen_endpoint = value;
    else if (strcmp(flag, "--kv-advertise-endpoint") == 0)
        o->advertise_endpoint = value;
    else if (strcmp(flag, "--kv-transport") == 0) {
        if (mi_remote_transport_parse(value, &o->transport_mode) != KVF_OK)
            nv_fatal("--kv-transport must be tcp or rdma");
    }
    else if (strcmp(flag, "--kv-tenant") == 0)
        o->tenant = value;
    else if (strcmp(flag, "--kv-page-size") == 0)
        o->page_size = (unsigned)integer(value);
    else if (strcmp(flag, "--kv-cache-capacity") == 0)
        o->cache_capacity = (unsigned)integer(value);
    else if (strcmp(flag, "--kv-prefetch-timeout") == 0)
        o->prefetch_timeout_ms = (unsigned)integer(value);
    else if (strcmp(flag, "--kv-worker-name") == 0)
        o->worker_name = value;
    else if (strcmp(flag, "--kv-tp-size") == 0)
        o->tp_size = (unsigned)integer(value);
    else if (strcmp(flag, "--kv-pp-size") == 0)
        o->pp_size = (unsigned)integer(value);
    else
        return 0;
    return 1;
}

static void print_remote_stats(NvEngine *engine, int trace) {
    NvRemoteStats stats;
    nv_engine_remote_flush(engine);
    nv_engine_remote_stats(engine, &stats);
    const char *active = stats.active_transport ? stats.active_transport : "OFF";
    if (trace)
        fprintf(stderr, "active_transport: %s\n", active);
    fprintf(stderr,
            "remote_lookup_requests=%" PRIu64 " remote_lookup_hits=%" PRIu64
            " remote_hit_pages=%" PRIu64 " remote_hit_tokens=%" PRIu64
            " remote_import_bytes=%" PRIu64 " remote_prefetch_requests=%" PRIu64
            " remote_prefetch_failures=%" PRIu64 " remote_prefetch_cancelled=%" PRIu64
            " remote_admission_skips=%" PRIu64 " remote_publish_retries=%" PRIu64 " remote_publish_failures=%" PRIu64
            " remote_prefill_skipped_tokens=%" PRIu64 " remote_prefill_fallbacks=%" PRIu64
            " remote_cache_evictions=%" PRIu64 " remote_transfer_latency_ns=%" PRIu64
            " remote_transfer_samples=%" PRIu64 " remote_published_pages=%" PRIu32 " active_transport=%s\n",
            stats.remote_lookup_requests, stats.remote_lookup_hits, stats.remote_hit_pages,
            stats.remote_hit_tokens, stats.remote_import_bytes, stats.remote_prefetch_requests,
            stats.remote_prefetch_failures, stats.remote_prefetch_cancelled,
            stats.remote_admission_skips, stats.remote_publish_retries, stats.remote_publish_failures,
            stats.remote_prefill_skipped_tokens, stats.remote_prefill_fallbacks,
            stats.remote_cache_evictions, stats.remote_transfer_latency_ns, stats.remote_transfer_samples,
            stats.remote_published_pages, active);
}
#endif

#ifdef NANO_WITH_STORAGE
static int parse_flag(StorageOptions *options, const char *flag) {
    if (strcmp(flag, "--spdk-no-pci") != 0)
        return 0;
    options->spdk.no_pci = options->spdk.no_huge = 1;
    return 1;
}

static int parse_value(StorageOptions *options, const char *flag, const char *value) {
    if (strcmp(flag, "--ssd-slots") == 0)
        options->ssd_slots = integer(value);
    else if (strcmp(flag, "--ssd-mode") == 0)
        options->ssd_mode = value;
    else if (strcmp(flag, "--ssd-dir") == 0)
        options->ssd_dir = value;
    else if (strcmp(flag, "--spdk-config") == 0)
        options->spdk.json_config = value;
    else if (strcmp(flag, "--spdk-bdev") == 0)
        options->spdk.bdev_name = value;
    else if (strcmp(flag, "--spdk-reactor-mask") == 0)
        options->spdk.reactor_mask = value;
    else if (strcmp(flag, "--spdk-iova-mode") == 0) {
        if (strcmp(value, "va") && strcmp(value, "pa"))
            nv_fatal("SPDK IOVA mode must be va or pa");
        options->spdk.iova_mode = value;
    }
    else if (strcmp(flag, "--spdk-base-virtaddr") == 0) {
        char *end;
        errno = 0;
        uintmax_t address = strtoumax(value, &end, 0);
        if (errno || *value < '0' || *value > '9' || *end || address > UINT64_MAX)
            nv_fatal("expected a nonnegative base virtual address");
        options->spdk.base_virtaddr = (uint64_t)address;
    }
    else if (strcmp(flag, "--spdk-offset") == 0) {
        char *end;
        errno = 0;
        uintmax_t offset = strtoumax(value, &end, 10);
        if (errno || *value < '0' || *value > '9' || *end || offset > UINT64_MAX)
            nv_fatal("expected a nonnegative byte offset");
        options->spdk.offset_bytes = (uint64_t)offset;
        options->spdk_offset_set = 1;
    } else {
        return 0;
    }
    return 1;
}

static void validate(const StorageOptions *options) {
    const char *mode = options->ssd_mode;
    if (strcmp(mode, "posix") && strcmp(mode, "uring") && strcmp(mode, "spdk"))
        nv_fatal("SSD mode must be posix, uring, or spdk");
    if (options->ssd_slots && !strcmp(mode, "spdk") &&
        (!options->spdk.json_config || !options->spdk.bdev_name || !options->spdk_offset_set))
        nv_fatal("SPDK requires --spdk-config, --spdk-bdev and --spdk-offset for a dedicated region");
}

/* 按后端计算槽步长，打开 SSD 后接入前缀缓存。 */
static void attach_ssd(NvCache *cache, const StorageOptions *storage) {
    if (!storage->ssd_slots)
        return;
    if (!cache->enabled)
        nv_fatal("SSD spill requires prefix reuse");
    size_t payload = 2 * sizeof(float) * nv_cache_block_elements(cache);
    if (payload > SIZE_MAX - 64 - 4095)
        nv_fatal("SSD slot size overflow");
    int is_spdk = strcmp(storage->ssd_mode, "spdk") == 0;
    size_t record = payload + 64;
    if (is_spdk)
        record = (record + 4095) & ~(size_t)4095;
    if ((size_t)storage->ssd_slots > SIZE_MAX / record)
        nv_fatal("SSD capacity overflow");

    mi_storage_backend *backend;
    mi_status status;
    if (is_spdk) {
        mi_spdk_config config = storage->spdk;
        config.capacity_bytes = record * storage->ssd_slots;
        status = mi_ssd_spdk_backend_create(config, payload, MI_SSD_VERIFY_ON_READ, &backend);
    } else {
        mi_ssd_file_mode mode = strcmp(storage->ssd_mode, "uring") == 0 ? MI_SSD_FILE_URING : MI_SSD_FILE_POSIX;
        status = mi_ssd_temp_backend_create(storage->ssd_dir, payload, record * storage->ssd_slots,
                                            MI_SSD_VERIFY_ON_READ, mode, 8, &backend);
    }
    if (status != MI_OK)
        nv_fatal(mi_status_string(status));
    nv_cache_attach_ssd(cache, backend, storage->ssd_slots);
}

static void print_stats(const StorageOptions *options, const NvCache *cache) {
    if (!options->ssd_slots)
        return;
    uint64_t reads, writes;
    nv_spill_stats(cache, &reads, &writes);
    fprintf(stderr, "ssd_mode=%s ssd_reads=%" PRIu64 " ssd_writes=%" PRIu64 "\n", options->ssd_mode, reads, writes);
}

#endif

static float number(const char *s) {
    char *end;
    errno = 0;
    float n = strtof(s, &end);
    if (errno || !*s || *end || !isfinite(n)) nv_fatal("expected finite numeric option");
    return n;
}

static double seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void print_tokens(const NvTokenizer *t, const NvSequence *s, int first, int ids) {
    for (int i = first; i < s->num_tokens; ++i) {
        int token = s->tokens[i];
        if (ids)
            printf("%s%d", i == first ? "" : " ", token);
        else if (token != 1 && token != 2)
            nv_print_piece(stdout, nv_decode(t, s->tokens[i - 1], token));
    }
}

static int run_prompts(NvEngine *e, const NvTokenizer *t, const char *const *prompts, int count,
                       int max_new, NvSampling sampling, int trace, int ids) {
    uint64_t submitted = trace ? (uint64_t)(seconds() * 1e9) : 0;
    for (int i = 0; i < count; ++i) {
        int n;
        int *tokens = nv_encode(t, prompts[i], &n);
        int id = nv_engine_add(e, tokens, n, max_new, sampling);
        free(tokens);
        if (id < 0)
            return 0;
    }

    int stream = count == 1 && !ids;
    NvSequence *lead = e->requests[0];
    int printed = lead->prompt_len;
    if (stream) {
        nv_print_piece(stdout, prompts[0]);
        fflush(stdout);
    }
    uint64_t generated = e->generated_tokens, forwards = e->forward_tokens, reused = e->cache.hits;
    uint64_t preemptions = e->preemptions, prefilled = e->prefill_tokens;
    double start = seconds();
    while (nv_engine_has_work(e)) {
        if (!nv_engine_step(e)) {
            nv_engine_wait(e);
            continue;
        }
        if (trace)
            fprintf(stderr, "step=%" PRIu64 " forwards=%" PRIu64 " generated=%" PRIu64
                    " cache_hits=%" PRIu64 " preemptions=%" PRIu64 "\n",
                    e->steps, e->forward_tokens, e->generated_tokens, e->cache.hits, e->preemptions);
        if (stream) {
            print_tokens(t, lead, printed, 0);
            printed = lead->num_tokens;
            fflush(stdout);
        }
    }
    double elapsed = seconds() - start;
    if (trace) {
#ifdef NANO_WITH_REMOTE_KV
        nv_engine_remote_flush(e);
#endif
        for (int i = 0; i < count; ++i) {
            NvSequence *s = e->requests[i];
            const NvRemoteTiming *t = &s->remote_timing;
            fprintf(stderr, "request=%d ttft_ns=%" PRIu64, s->id,
                    s->first_token_ns ? s->first_token_ns - submitted : 0);
#define TRACE(field) fprintf(stderr, " " #field "=%" PRIu64, t->field)
            TRACE(dir_lookup_ns);
            TRACE(conn_acquire_ns);
            TRACE(net_recv_ns);
            TRACE(crc_verify_ns);
            TRACE(cpu_stage_ns);
            TRACE(cuda_h2d_ns);
            TRACE(cuda_sync_ns);
            TRACE(kv_commit_ns);
            TRACE(sched_resume_ns);
            TRACE(first_decode_ns);
            TRACE(prefetch_queue_ns);
            TRACE(lease_check_ns);
            TRACE(received_bytes);
            TRACE(data_requests);
#undef TRACE
            fputc('\n', stderr);
        }
    }
    if (stream) {
        fputc('\n', stdout);
    } else {
        for (int i = 0; i < count; ++i) {
            printf("[%d] ", i);
            if (!ids)
                nv_print_piece(stdout, prompts[i]);
            print_tokens(t, e->requests[i], e->requests[i]->prompt_len, ids);
            fputc('\n', stdout);
        }
    }
    generated = e->generated_tokens - generated;
    fprintf(stderr, "generated=%" PRIu64 " forwards=%" PRIu64 " reused=%" PRIu64
            " preemptions=%" PRIu64 " elapsed=%.3fs output_tok/s=%.2f prefilled=%" PRIu64 "\n",
            generated, e->forward_tokens - forwards, e->cache.hits - reused,
            e->preemptions - preemptions, elapsed, elapsed > 0 ? (double)generated / elapsed : 0,
            e->prefill_tokens - prefilled);
    return 1;
}

int nv_cli_run(int argc, char **argv) {

    if (argc == 2 && strcmp(argv[1], "--help") == 0) { usage(stdout); return 0; }

    if (argc < 2) { usage(stderr); return 1; }
    const char *tokenizer_path = NV_DEFAULT_TOKENIZER_PATH;
    const char *device = "cpu";
#ifdef NANO_WITH_REMOTE_KV
    NvRemoteOptions remote;
    nv_remote_options_defaults(&remote);
    int remote_enabled = 0, remote_configured = 0, block_size_set = 0;
#endif
#ifdef NANO_WITH_STORAGE
    StorageOptions storage = {.ssd_mode = "posix", .ssd_dir = "/tmp"};
#endif

    const char **prompts = nv_alloc((size_t)argc, sizeof(char *));
    int count = 0, max_new = NV_DEFAULT_MAX_NEW_TOKENS;
    int trace = NV_DEFAULT_TRACE, ids = NV_DEFAULT_IDS, read_stdin = 0;

    NvOptions options = {.num_blocks = NV_DEFAULT_KV_BLOCKS, .block_size = NV_DEFAULT_KV_BLOCK_SIZE,
        .max_sequences = NV_DEFAULT_MAX_SEQUENCES, .token_budget = NV_DEFAULT_TOKEN_BUDGET,
        .prefix_cache = NV_DEFAULT_PREFIX_CACHE};

    NvSampling sampling = {NV_DEFAULT_TEMPERATURE, NV_DEFAULT_TOP_P,
                           NV_DEFAULT_SEED, NV_DEFAULT_IGNORE_EOS};

    for (int i = 2; i < argc; i++) {

        const char *flag = argv[i];

        if (strcmp(flag, "--no-cache") == 0) { options.prefix_cache = 0; continue; }
        if (strcmp(flag, "--trace") == 0) { trace = 1; continue; }
        if (strcmp(flag, "--ids") == 0) { ids = 1; continue; }
        if (strcmp(flag, "--stdin") == 0) { read_stdin = 1; continue; }
        if (strcmp(flag, "--ignore-eos") == 0) { sampling.ignore_eos = 1; continue; }
#ifdef NANO_WITH_REMOTE_KV
        if (strcmp(flag, "--kv-publish-generated") == 0) {
            remote.publish_generated = remote_configured = 1;
            continue;
        }
        if (!strcmp(flag, "--kv-rdma-fallback") || !strcmp(flag, "--no-kv-rdma-fallback")) {
            remote.rdma_strict = !strcmp(flag, "--no-kv-rdma-fallback");
            remote_configured = 1;
            continue;
        }
#endif
        if (strcmp(flag, "--remote-kv") == 0) {
#ifdef NANO_WITH_REMOTE_KV
            remote_enabled = 1;
            continue;
#else
            nv_fatal("remote KV support is disabled in this build");
#endif
        }
#ifdef NANO_WITH_STORAGE
        if (parse_flag(&storage, flag)) continue;
#endif

        if (i + 1 >= argc) nv_fatal("option needs a value; see --help");
        const char *value = argv[++i];
        if (strcmp(flag, "-z") == 0) { tokenizer_path = value; continue; }
        if (strcmp(flag, "--device") == 0) { device = value; continue; }

        if (strcmp(flag, "-i") == 0) { prompts[count++] = value; continue; }
        if (strcmp(flag, "-n") == 0) { max_new = integer(value); continue; }
        if (strcmp(flag, "-t") == 0) { sampling.temperature = number(value); continue; }
        if (strcmp(flag, "-p") == 0) { sampling.top_p = number(value); continue; }
        if (strcmp(flag, "-s") == 0) { sampling.seed = (uint64_t)integer(value); continue; }
        if (strcmp(flag, "--blocks") == 0) { options.num_blocks = integer(value); continue; }
        if (strcmp(flag, "--block-size") == 0) {
            options.block_size = integer(value);
#ifdef NANO_WITH_REMOTE_KV
            block_size_set = 1;
#endif
            continue;
        }
        if (strcmp(flag, "--batch") == 0) { options.max_sequences = integer(value); continue; }
        if (strcmp(flag, "--budget") == 0) { options.token_budget = integer(value); continue; }
#ifdef NANO_WITH_REMOTE_KV
        if (parse_remote(&remote, flag, value)) { remote_configured = 1; continue; }
#endif
#ifdef NANO_WITH_STORAGE
        if (parse_value(&storage, flag, value)) continue;
#endif
        nv_fatal("unknown option; see --help");
    }

    if (sampling.temperature < 0 || sampling.top_p <= 0 || sampling.top_p > 1)
        nv_fatal("temperature must be >=0, top-p must be in (0,1]");
#ifdef NANO_WITH_REMOTE_KV
    if (remote_configured && !remote_enabled)
        nv_fatal("remote KV options require --remote-kv");
    if (remote_enabled && (!options.prefix_cache || remote.tp_size != 1 || remote.pp_size != 1))
        nv_fatal("remote KV requires prefix reuse, TP=1 and PP=1");
    if (remote.page_size) {
        if (block_size_set && remote.page_size != (unsigned)options.block_size)
            nv_fatal("--block-size and --kv-page-size must match");
        options.block_size = (int)remote.page_size;
    }
#endif

    if (read_stdin && count)
        nv_fatal("--stdin and -i cannot be combined");
    if (!read_stdin && !count)
        prompts[count++] = NV_DEFAULT_PROMPT;
    options.max_requests = read_stdin ? 1 : count;
    NvModel model;
#ifdef NANO_WITH_STORAGE
    validate(&storage);
    void *mapping;
    size_t bytes;
    mi_status st = mi_model_map(argv[1], sizeof(NvConfig), &mapping, &bytes);
    if (st != MI_OK)
        nv_fatal(mi_status_string(st));
    nv_model_load_mapping(&model, mapping, bytes);
#else
    nv_model_load(&model, argv[1]);
#endif
    NvTokenizer tokenizer;
    nv_tokenizer_load(&tokenizer, tokenizer_path, model.config.vocab_size);
    NvEngine engine;
    if (strcmp(device, "cpu") == 0) {
#ifdef NV_USE_MINIINFER_POOL
        options.pooled_buffers = 1;
#endif
        nv_engine_init(&engine, &model, options);
    } else if (strcmp(device, "cuda") == 0) {
#ifdef NANO_WITH_CUDA
#ifdef NV_USE_MINIINFER_CUDA_POOL
        options.pooled_buffers = 1;
#endif
        nv_engine_init_cuda(&engine, &model, options, 0);
#else
        nv_fatal("CUDA support is disabled in this build");
#endif
    } else {
        nv_fatal("device must be cpu or cuda");
    }

#ifdef NANO_WITH_STORAGE
    attach_ssd(&engine.cache, &storage);
#endif
    char *line = NULL;
    int ok;
#ifdef NANO_WITH_REMOTE_KV
    if (remote_enabled) {
        int status = nv_engine_enable_remote(&engine, &tokenizer, remote);
        if (status < 0)
            nv_fatal("invalid remote KV options or unsupported layout; see --help");
        if (!status && remote.transport_mode == KVF_TRANSPORT_RDMA_RSOCKET && remote.rdma_strict) {
            fputs("remote KV could not start with required RDMA transport\n", stderr);
            ok = 0;
            goto done;
        }
        if (!status)
            fputs("remote KV unavailable; using local prefill\n", stderr);
    }
#endif

    fprintf(stderr, "model: dim=%d layers=%d heads=%d kv_heads=%d vocab=%d context=%d\n",
            model.config.dim, model.config.n_layers, model.config.n_heads,
            model.config.n_kv_heads, model.config.vocab_size, model.config.seq_len);

    size_t kv_bytes = (size_t)options.num_blocks * (size_t)options.block_size *
                      (size_t)model.config.n_layers * (size_t)engine.cache.kv_dim * 2 * sizeof(float);
    fprintf(stderr, "KV pool: %.2f MiB; requests=%d device=%s\n",
            (double)kv_bytes / 1048576.0, options.max_requests, device);

    size_t line_capacity = 0;
    do {
        if (read_stdin) {
            ssize_t length = getline(&line, &line_capacity, stdin);
            if (length < 0)
                break;
            while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
                line[--length] = '\0';
            prompts[0] = line;
            count = 1;
        }
        if (!run_prompts(&engine, &tokenizer, prompts, count, max_new, sampling, trace, ids)) {
            const char *error = "request exceeds context or KV pool capacity; reduce -n/prompt or increase --blocks";
            if (!read_stdin)
                nv_fatal(error);
            fprintf(stderr, "%s: %s\n", cli_name, error);
            continue;
        }
#ifdef NANO_WITH_STORAGE
        print_stats(&storage, &engine.cache);
#endif
#ifdef NANO_WITH_REMOTE_KV
        if (remote_enabled)
            print_remote_stats(&engine, trace);
#endif
        fflush(stdout);
        (void)nv_engine_clear_requests(&engine);
    } while (read_stdin);

    ok = !read_stdin || !ferror(stdin);
#ifdef NANO_WITH_REMOTE_KV
done:
#endif
    free(line);
    nv_engine_free(&engine);
    nv_tokenizer_free(&tokenizer);
    nv_model_free(&model);
    free(prompts);
    return ok ? 0 : 1;
}
