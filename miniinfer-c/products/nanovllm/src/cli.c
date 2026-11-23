/*
 * 单机推理的参数、交互循环和运行时资源装配。
 * CLI 拥有模型、词表、引擎和可选本地 SSD。命令循环单线程驱动。
 */
#define _POSIX_C_SOURCE 200809L
#include "nano_cache.h"
#include "nano_cli.h"
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
#ifdef NANO_WITH_STORAGE
    fputs(
            "  --ssd-slots N number of spill slots (default 0, disabled)\n"
            "  --ssd-mode posix|uring|spdk (default posix)\n"
            "  --ssd-dir DIR directory for the private POSIX/io_uring spill file\n"
            "  --spdk-config PATH --spdk-bdev NAME --spdk-offset BYTES\n"
            "                explicit bdev and dedicated byte region\n"
            "  --spdk-no-pci use the SPDK software bdev environment\n"
            "  --spdk-reactor-mask MASK (default 0x1)\n", file);
#endif
}

static int integer(const char *s) {
    char *end;
    errno = 0;
    long n = strtol(s, &end, 10);
    if (errno || !*s || *end || n < 1 || n > INT_MAX) nv_fatal("expected positive integer option");
    return (int)n;
}

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
    size_t payload = 2 * sizeof(float) * (cache->elements / (size_t)cache->num_blocks);
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

int nv_cli_run(int argc, char **argv) {

    if (argc == 2 && strcmp(argv[1], "--help") == 0) { usage(stdout); return 0; }

    if (argc < 2) { usage(stderr); return 1; }
    const char *tokenizer_path = NV_DEFAULT_TOKENIZER_PATH;
    const char *device = "cpu";
#ifdef NANO_WITH_STORAGE
    StorageOptions storage = {.ssd_mode = "posix", .ssd_dir = "/tmp"};
#endif

    const char **prompts = nv_alloc((size_t)argc, sizeof(char *));
    int count = 0, max_new = NV_DEFAULT_MAX_NEW_TOKENS;
    int trace = NV_DEFAULT_TRACE, ids = NV_DEFAULT_IDS;

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
        if (strcmp(flag, "--ignore-eos") == 0) { sampling.ignore_eos = 1; continue; }
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
        if (strcmp(flag, "--block-size") == 0) { options.block_size = integer(value); continue; }
        if (strcmp(flag, "--batch") == 0) { options.max_sequences = integer(value); continue; }
        if (strcmp(flag, "--budget") == 0) { options.token_budget = integer(value); continue; }
#ifdef NANO_WITH_STORAGE
        if (parse_value(&storage, flag, value)) continue;
#endif
        nv_fatal("unknown option; see --help");
    }

    if (sampling.temperature < 0 || sampling.top_p <= 0 || sampling.top_p > 1)
        nv_fatal("temperature must be >=0, top-p must be in (0,1]");

    if (!count) prompts[count++] = NV_DEFAULT_PROMPT;
    /* CLI 已知输入总数，按需设置请求表容量。 */
    options.max_requests = count;
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

    for (int i = 0; i < count; i++) {
        int n;

        int *tokens = nv_encode(&tokenizer, prompts[i], &n);

        if (nv_engine_add(&engine, tokens, n, max_new, sampling) < 0)
            nv_fatal("request exceeds context or KV pool capacity; reduce -n/prompt or increase --blocks");

        free(tokens);
    }

    fprintf(stderr, "model: dim=%d layers=%d heads=%d kv_heads=%d vocab=%d context=%d\n",
            model.config.dim, model.config.n_layers, model.config.n_heads,
            model.config.n_kv_heads, model.config.vocab_size, model.config.seq_len);

    size_t kv_bytes = (size_t)options.num_blocks * (size_t)options.block_size *
                      (size_t)model.config.n_layers * (size_t)engine.cache.kv_dim * 2 * sizeof(float);
    fprintf(stderr, "KV pool: %.2f MiB; requests=%d device=%s\n",
            (double)kv_bytes / 1048576.0, count, device);

    int printed = engine.requests[0]->prompt_len;
    if (count == 1 && !ids) { nv_print_piece(stdout, prompts[0]); fflush(stdout); }

    double start = seconds();

    /* 每轮推进请求，单请求模式下及时输出新 token。 */
    while (nv_engine_step(&engine)) {
        if (trace) fprintf(stderr, "step=%" PRIu64 " forwards=%" PRIu64 " generated=%" PRIu64
                           " cache_hits=%" PRIu64 " preemptions=%" PRIu64 "\n",
                           engine.steps, engine.forward_tokens, engine.generated_tokens,
                           engine.cache.hits, engine.preemptions);

        if (count == 1 && !ids) {
            NvSequence *s = engine.requests[0];

            for (; printed < s->num_tokens; printed++) {
                int token = s->tokens[printed];
                if (token != 1 && token != 2) nv_print_piece(stdout, nv_decode(&tokenizer, s->tokens[printed - 1], token));
            }

            fflush(stdout);
        }
    }
    double elapsed = seconds() - start;
    if (count == 1 && !ids) fputc('\n', stdout);

    if (count > 1 || ids) {
        for (int i = 0; i < count; i++) {
            NvSequence *s = engine.requests[i];
            printf("[%d] ", i);
            if (!ids) nv_print_piece(stdout, prompts[i]);
            for (int j = s->prompt_len; j < s->num_tokens; j++) {

                if (ids) { printf("%s%d", j == s->prompt_len ? "" : " ", s->tokens[j]); continue; }
                if (s->tokens[j] != 1 && s->tokens[j] != 2)
                    nv_print_piece(stdout, nv_decode(&tokenizer, s->tokens[j - 1], s->tokens[j]));
            }
            fputc('\n', stdout);
        }
    }

    fprintf(stderr, "generated=%" PRIu64 " forwards=%" PRIu64 " reused=%" PRIu64
            " preemptions=%" PRIu64 " elapsed=%.3fs output_tok/s=%.2f\n",
            engine.generated_tokens, engine.forward_tokens, engine.cache.hits,
            engine.preemptions, elapsed, elapsed > 0 ? (double)engine.generated_tokens / elapsed : 0);
#ifdef NANO_WITH_STORAGE
    print_stats(&storage, &engine.cache);
#endif

    nv_engine_free(&engine);
    nv_tokenizer_free(&tokenizer);
    nv_model_free(&model);
    free(prompts);
    return 0;
}
