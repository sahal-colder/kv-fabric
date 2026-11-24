# KV-Fabric 简历基准复现

下面的命令在 Linux 或 WSL2 中运行。公开 TCP 基线需要 CUDA Toolkit 12.8、CMake、Ninja、OpenSSL、liburing 和 Python 3。模型封装和请求生成依赖 `numpy` 与 `tokenizers`。CPU Sanitizer 使用 Clang。验证模型转换另需支持 Qwen2 的 PyTorch 与 Transformers。

归档样本使用 `06b7f28` 及当日工作区源码。`source-snapshot.csv` 记录了该快照的源文件哈希，包含当时工作区的 rsockets 改动。以下命令使用公开提交 `06b7f28` 的 TCP 路径复跑同模型、输入和测点。若保有与哈希相符的实测快照，可将其复制到 `$WORK/source` 以采用当时的实现，并安装 rsockets 开发库。Qwen 前向适配、计时桩和逐层 RPC 开关仅应用于基准副本。第 D 组面向未加计时桩的源码。

## 准备独立目录

从当前仓库根目录运行。将 `REPRO` 指向本文件，`ASSETS` 指向包含 `config.json`、`tokenizer.json`、`model.safetensors.index.json` 与两个 Safetensors 分片的本地 Qwen2.5-3B-Instruct 目录。原始文件的 SHA-256 见同目录 `model-provenance.csv`。

```bash
REPRO="$PWD/docs/benchmarks/resume-eval/REPRODUCE.md"
ASSETS=/path/to/Qwen2.5-3B-Instruct
WORK=$(mktemp -d /tmp/kvf-resume-eval.XXXXXX)
test "$(findmnt -n -o FSTYPE --target "$WORK")" = ext4
mkdir "$WORK/source" "$WORK/raw"
git -c safe.directory="$PWD" archive 06b7f286d13c20c26e01803d7c43fb09adcd83eb \
  CMakeLists.txt src include tools tests cmake config examples third_party |
  tar -xf - -C "$WORK/source"
cp -a "$WORK/source" "$WORK/bench-source"
RDMA_FLAGS=()
if test -f "$WORK/source/src/worker/remote/remote_sock.c"; then RDMA_FLAGS+=(-DMINIINFER_WITH_RDMA=ON); fi

python3 - "$REPRO" "$WORK" <<'PY'
import re, sys
from pathlib import Path
source = Path(sys.argv[1]).read_text(encoding='utf-8')
pattern = r'<!-- bench-file: ([a-z0-9.-]+) -->\n```(?:python|bash)\n(.*?)\n```'
for name, code in re.findall(pattern, source, flags=re.S):
    (Path(sys.argv[2]) / name).write_text(code+'\n', encoding='utf-8')
PY

python3 "$WORK/prepare.py" "$WORK/bench-source"
python3 "$WORK/qwen-adapter.py" "$WORK/bench-source"
python3 "$WORK/pack-qwen.py" "$ASSETS" "$WORK"
```

## A 和 B 的推理基准

设备编号是 `0`，单卡运行发布和消费进程。两组均使用 TCP 回环，依次执行。若使用不同 GPU，将 `CMAKE_CUDA_ARCHITECTURES` 和编译器路径替换为该机器的配置。

```bash
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 PYTHONDONTWRITEBYTECODE=1
cmake -S "$WORK/bench-source" -B "$WORK/bench-cuda" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMINIINFER_WITH_URING=ON "${RDMA_FLAGS[@]}" \
  -DMINIINFER_WITH_CUDA=ON -DNANO_WITH_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=120 -DCMAKE_CUDA_COMPILER=/usr/local/cuda-12.8/bin/nvcc
cmake --build "$WORK/bench-cuda" --target kvfabricd test_nano_remote_worker kvf_bench -j 8
nvidia-smi --query-gpu=name,memory.total,memory.free,memory.used,temperature.gpu,driver_version --format=csv >"$WORK/gpu-before.csv"
python3 "$WORK/run-eval.py" --work "$WORK" --assets "$ASSETS" \
  --experiment a --rounds 30 --shard 10 --legacy-rounds 5 --out "$WORK/raw/a"
python3 "$WORK/run-eval.py" --work "$WORK" --assets "$ASSETS" \
  --experiment b --rounds 16 --out "$WORK/raw/b"
nvidia-smi --query-gpu=name,memory.total,memory.free,memory.used,temperature.gpu,driver_version --format=csv >"$WORK/gpu-after.csv"
```

## C 的存储基准

4 MiB、32 并发的对齐缓冲超过默认的 64 MiB 锁页额度。命令只将基准子进程的 `RLIMIT_MEMLOCK` 调到 256 MiB，并用普通用户身份运行 `kvf_bench`。使用本机 ext4 临时文件，运行日志会检查两组实际启用的后端。

```bash
bash "$WORK/run-ssd.sh" "$WORK"
```

## D 的已注册回归

CPU Release、CUDA Release 和 CPU Debug Sanitizer 从原始源码副本构建。CUDA 配置沿用本次实测卡的 Toolkit 与 SM 版本。

```bash
BASE=(-G Ninja -DMINIINFER_WITH_URING=ON "${RDMA_FLAGS[@]}")
cmake -S "$WORK/source" -B "$WORK/release-cpu" "${BASE[@]}" -DCMAKE_BUILD_TYPE=Release
cmake -S "$WORK/source" -B "$WORK/release-cuda" "${BASE[@]}" -DCMAKE_BUILD_TYPE=Release \
  -DMINIINFER_WITH_CUDA=ON -DNANO_WITH_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=120 -DCMAKE_CUDA_COMPILER=/usr/local/cuda-12.8/bin/nvcc
cmake -S "$WORK/source" -B "$WORK/asan-cpu" "${BASE[@]}" -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DKVF_SANITIZE=ON
for build in release-cpu release-cuda asan-cpu; do
  cmake --build "$WORK/$build" -j 6
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    ctest --test-dir "$WORK/$build" --output-on-failure -j 2 >"$WORK/$build-ctest.log"
done
```

## 汇总与模型校验

原始 CSV 保留请求粒度的时间、实际命中计数、生成 ID 和适用的 KV 页摘要。汇总前校验样本口径与生成一致性。

```bash
mkdir "$WORK/results"
cp "$WORK/raw/a/"*.csv "$WORK/raw/b/"*.csv "$WORK/raw/c/"*.csv "$WORK/results/"
python3 "$WORK/summarize.py" "$WORK/results"
python3 "$WORK/metadata.py" "$WORK" "$ASSETS" "$WORK/results"
```

使用带 PyTorch、Transformers 和 Accelerate 的 Python 环境另跑转换核验。单独运行该命令，空出 GPU 给 FP32 参考模型。

```bash
python "$WORK/verify-qwen.py" "$ASSETS" "$WORK/results" "$WORK/results/qwen-reference.csv"
```

## 基准脚本与模型适配

<!-- bench-file: prepare.py -->
```python
from pathlib import Path
import sys

root = Path(sys.argv[1])

def replace(path, old, new):
    file = root / path
    source = file.read_text(encoding="utf-8")
    assert source.count(old) == 1, (path, old)
    file.write_text(source.replace(old, new), encoding="utf-8")

worker = "tests/nanovllm/remote_worker.c"
transfer = "src/worker/remote/transfer.c"
rsockets = (root / "src/worker/remote/remote_sock.c").exists()
replace("tests/nanovllm/test_remote_inference.py", "self.output.get(timeout=25)", "self.output.get(timeout=60)")
replace("include/nanovllm/nv.h", "first_decode_ns, ttft_ns;", "first_decode_ns, ttft_ns, eval_reused_tokens;")
replace("src/nanovllm/nano_remote_kv.c", "        r->stats.remote_prefill_skipped_tokens += (uint64_t)pages * n;",
        "        t->eval_reused_tokens = (uint64_t)pages * n;\n"
        "        r->stats.remote_prefill_skipped_tokens += (uint64_t)pages * n;")
replace("src/worker/remote/cache.c", '#include <string.h>', '#include <stdlib.h>\n#include <string.h>')
replace("src/worker/remote/cache.c", "? resolve_batch(w, *out, tokens, count, result, suffix, cancel)",
        "    && !getenv(\"KVF_RESUME_EVAL_LEGACY\")\n        ? resolve_batch(w, *out, tokens, count, result, suffix, cancel)")
replace("src/worker/remote/cache.c", "mi_remote_import(w, s, page, &g, suffix.deadline_ms, cancel)",
        "mi_remote_import(w, s, page, &g, suffix.deadline_ms, cancel, &result->timing)")
for path in ("src/worker/remote/internal.h", transfer):
    source = (root / path).read_text(encoding="utf-8")
    start = source.index("kvf_status mi_remote_import(")
    end = source.index("const atomic_bool *cancel)", start)
    (root / path).write_text(source[:end] + source[end:].replace(
        "const atomic_bool *cancel)", "const atomic_bool *cancel, mi_remote_timing *timing)", 1),
        encoding="utf-8")
if rsockets:
    before_fetch = (
        "        st = remote_fetch(socket, peer ? KVF_FETCH_STREAM : KVF_FETCH, "
        "&request, dst, r.layer.size, end, NULL);")
    timed_fetch = """        st = remote_fetch(socket, peer ? KVF_FETCH_STREAM : KVF_FETCH, &request, dst, r.layer.size,
                          end, timing ? &wire : NULL);"""
else:
    # Preserve the public TCP client's validation while collecting frame timing.
    client = "src/control/client.c"
    replace(client, "void *payload, size_t capacity) {\n    if (!r->layer.size",
            "void *payload, size_t capacity, kvf_receive_timing *timing) {\n    if (!r->layer.size")
    replace(client, "kvf_rpc_on(fd, KVF_FETCH_STREAM, &w, payload, r->layer.size, &n, deadline)",
            "kvf_rpc_on_timed(fd, KVF_FETCH_STREAM, &w, payload, r->layer.size, &n, deadline, timing)")
    replace(client, "return fetch((kvf_client){ep, timeout_ms}, -1, 0, r, payload, capacity);",
            "return fetch((kvf_client){ep, timeout_ms}, -1, 0, r, payload, capacity, NULL);")
    replace(client, "return fetch((kvf_client){0}, fd, deadline, r, payload, capacity);",
            """return fetch((kvf_client){0}, fd, deadline, r, payload, capacity, NULL);
}

kvf_status kvf_client_fetch_on_timed(int fd, uint64_t deadline, const kvf_fetch_request *r,
                                    void *payload, size_t capacity, kvf_receive_timing *timing) {
    return fetch((kvf_client){0}, fd, deadline, r, payload, capacity, timing);""")
    replace("include/kvfabric/client.h", "kvf_status kvf_client_fetch_batch_on(",
            """kvf_status kvf_client_fetch_on_timed(int fd, uint64_t deadline, const kvf_fetch_request *r,
                                    void *payload, size_t capacity, kvf_receive_timing *timing);
kvf_status kvf_client_fetch_batch_on(""")
    before_fetch = """        st = fd >= 0 ? kvf_client_fetch_on(fd, transfer_deadline(client, deadline_ms),
                                           &r, dst, lease->lease.size)
                     : kvf_client_fetch(g->endpoint, client.timeout_ms, &r, dst, lease->lease.size);"""
    timed_fetch = """        st = fd >= 0 ? kvf_client_fetch_on_timed(fd, transfer_deadline(client, deadline_ms),
                                                 &r, dst, lease->lease.size, timing ? &wire : NULL)
                     : kvf_client_fetch(g->endpoint, client.timeout_ms, &r, dst, lease->lease.size);"""
replace(transfer, before_fetch, f"""        kvf_receive_timing wire = {{0}};
        if (timing)
            ++timing->data_requests;
{timed_fetch}
        if (timing) {{
            timing->received_bytes += wire.received_bytes;
            timing->net_recv_ns += wire.net_recv_ns;
            timing->reply_wait_ns += wire.reply_wait_ns;
            timing->crc_verify_ns += wire.crc_verify_ns;
        }}""")
replace("tools/kvf_bench.c", "mi_remote_import(b->worker, sequence, 0, &grant, 0, NULL)",
        "mi_remote_import(b->worker, sequence, 0, &grant, 0, NULL, NULL)")

transport = 'after.active_transport ? after.active_transport : "OFF"' if rsockets else 'e->remote ? "TCP" : "OFF"'
replace(worker, "static void request(NvEngine", r'''static void eval_reset(NvEngine *e) {
    if (!nv_engine_clear_requests(e))
        nv_fatal("evaluation requires an idle worker");
    int capacity = e->cache.num_blocks;
    int *blocks = nv_alloc((size_t)capacity, sizeof(*blocks));
    /* Hold every block at once to exercise the real LRU eviction path. */
    for (int i = 0; i < capacity; ++i) {
        blocks[i] = nv_cache_allocate(&e->cache);
        if (blocks[i] < 0)
            nv_fatal("evaluation could not drain the local prefix cache");
    }
    for (int i = 0; i < capacity; ++i)
        nv_cache_discard_remote(&e->cache, blocks[i]);
    free(blocks);
    nv_engine_remote_flush(e);
    puts("{\"reset\":true}");
    fflush(stdout);
}

static void eval_request(NvEngine *e, int max_new, char *state, int hashes) {
    int *inputs[4], lengths[4], count = 0;
    for (char *line = strtok_r(NULL, "\t\r\n", &state); line;
         line = strtok_r(NULL, "\t\r\n", &state)) {
        if (count == 4)
            nv_fatal("evaluation supports at most four requests in a wave");
        inputs[count] = read_token_ids(line, e->model->config.seq_len, &lengths[count]);
        ++count;
    }
    if (!count || (hashes && count != 1) || !nv_engine_clear_requests(e))
        nv_fatal("invalid evaluation wave");
    uint64_t local = e->cache.hits, prefill = e->prefill_tokens;
    NvRemoteStats before, after;
    nv_engine_remote_stats(e, &before);
    NvSampling sampling = {.temperature = 0.0f, .top_p = 1.0f, .seed = 1234, .ignore_eos = 1};
    uint64_t start = now_ns();
    for (int i = 0; i < count; ++i)
        if (nv_engine_add(e, inputs[i], lengths[i], max_new, sampling) != i)
            nv_fatal("evaluation prompt exceeds context or KV pool");
    while (nv_engine_has_work(e))
        if (!nv_engine_step(e))
            nv_engine_wait(e);
    uint64_t elapsed = now_ns() - start;
    nv_engine_remote_flush(e);
    nv_engine_remote_stats(e, &after);
    printf("{\"batch_ns\":%" PRIu64 ",\"local_tokens\":%" PRIu64 ",\"prefilled\":%" PRIu64
           ",\"remote_tokens\":%" PRIu64 ",\"prefetch_requests\":%" PRIu64
           ",\"prefetch_failures\":%" PRIu64 ",\"fallbacks\":%" PRIu64 ",\"admission_skips\":%" PRIu64
           ",\"published\":%" PRIu32 ",\"transport\":\"%s\",\"items\":[",
           elapsed, e->cache.hits - local, e->prefill_tokens - prefill,
           after.remote_prefill_skipped_tokens - before.remote_prefill_skipped_tokens,
           after.remote_prefetch_requests - before.remote_prefetch_requests,
           after.remote_prefetch_failures - before.remote_prefetch_failures,
           after.remote_prefill_fallbacks - before.remote_prefill_fallbacks,
           after.remote_admission_skips - before.remote_admission_skips,
           after.remote_published_pages, EVAL_TRANSPORT);
    for (int i = 0; i < count; ++i) {
        NvSequence *seq = e->requests[i];
        const NvRemoteTiming *t = &seq->remote_timing;
        printf("%s{\"prompt_tokens\":%d,\"ttft_ns\":%" PRIu64 ",\"prefill_ns\":%" PRIu64
               ",\"reused_tokens\":%" PRIu64 ",\"data_requests\":%" PRIu64
               ",\"received_bytes\":%" PRIu64 ",\"net_recv_ns\":%" PRIu64
               ",\"dir_lookup_ns\":%" PRIu64 ",\"h2d_ns\":%" PRIu64 ",\"cuda_sync_ns\":%" PRIu64
               ",\"tokens\":[", i ? "," : "", seq->prompt_len, seq->first_token_ns - start,
               seq->prefill_done_ns - start, t->eval_reused_tokens, t->data_requests,
               t->received_bytes, t->net_recv_ns, t->dir_lookup_ns, t->cuda_h2d_ns, t->cuda_sync_ns);
        for (int j = seq->prompt_len; j < seq->num_tokens; ++j)
            printf("%s%d", j == seq->prompt_len ? "" : ",", seq->tokens[j]);
        fputc(']', stdout);
        if (hashes)
            print_page_hashes(&e->cache, seq);
        fputc('}', stdout);
        free(inputs[i]);
    }
    puts("]}");
    fflush(stdout);
}

static void request(NvEngine'''.replace("EVAL_TRANSPORT", transport))
replace(worker, '    const int cancel = strcmp(command, "cancel") == 0;',
        '''    if (strcmp(command, "eval-reset") == 0) {
        eval_reset(e);
        return;
    }
    if (strcmp(command, "eval-ids") == 0 || strcmp(command, "eval-hash") == 0) {
        eval_request(e, max_new, state, strcmp(command, "eval-hash") == 0);
        return;
    }
    const int cancel = strcmp(command, "cancel") == 0;''')

bench = "tools/kvf_bench.c"
replace(bench, "uint64_t latency[SAMPLES];", "uint64_t latency[SAMPLES], write_ns[SAMPLES], read_ns[SAMPLES];")
replace(bench, """    st = mi_backend_write_record(b->backend, slot, &record, b->expected);
    if (st == MI_OK)
        st = mi_backend_read_validated(b->backend, slot, &record, check->data);""",
        """    uint64_t write_start = time_ns(CLOCK_MONOTONIC);
    st = mi_backend_write_record(b->backend, slot, &record, b->expected);
    l->write_ns[sample] = time_ns(CLOCK_MONOTONIC) - write_start;
    if (st == MI_OK) {
        uint64_t read_start = time_ns(CLOCK_MONOTONIC);
        st = mi_backend_read_validated(b->backend, slot, &record, check->data);
        l->read_ns[sample] = time_ns(CLOCK_MONOTONIC) - read_start;
    }""")
replace(bench, r'''            if (fprintf(raw, "%s,%s,%zu,%u,%u,%u,%u,%" PRIu64 "\n", b->mode, b->device_name,
                        b->bytes, b->clients, round, i, j, duration) < 0)''',
        r'''            if (fprintf(raw, "%s,%s,%zu,%u,%u,%u,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                        b->mode, b->device_name, b->bytes, b->clients, round, i, j, duration,
                        lanes[i].write_ns[j], lanes[i].read_ns[j]) < 0)''')


```

<!-- bench-file: qwen-adapter.py -->
```python
from pathlib import Path
import sys

root = Path(sys.argv[1])
def change(path, old, new):
    path = root / path
    source = path.read_text(encoding="utf-8")
    assert source.count(old) == 1, (path, old)
    path.write_text(source.replace(old, new), encoding="utf-8")

change("include/nanovllm/nv.h", "    size_t mapping_size;\n    /*", "    size_t mapping_size;\n    const float *qwen_bf16_bias;\n    /*")
cu = "src/nanovllm/nano_cuda.cu"
change(cu, "#include <cuda_runtime.h>", "#include <cuda_runtime.h>\n#include <cuda_bf16.h>")
change(cu,
       "__global__ static void embed(float *x, const float *weight, const int *input, int dim)",
       """__device__ static float weight_value(float value) { return value; }
__device__ static float weight_value(__nv_bfloat16 value) { return __bfloat162float(value); }

template <class Weight>
__global__ static void embed(float *x, const Weight *weight, const int *input, int dim)""")
change(cu, "        x[i] = weight[(size_t)input[0] * dim + i];",
       "        x[i] = weight_value(weight[(size_t)input[0] * dim + i]);")
change(cu, "__global__ static void normalize(float *out, const float *x, const float *weight, int dim)",
       """template <class Weight>
__global__ static void normalize(float *out, const float *x, const Weight *weight, int dim)""")
change(cu, "rsqrtf(sum / dim + 1e-5f)", "rsqrtf(sum / dim + (sizeof(Weight) == 2 ? 1e-6f : 1e-5f))")
change(cu, "out[i] = (x[i] * scale) * weight[i];", "out[i] = (x[i] * scale) * weight_value(weight[i]);")
change(cu,
       """__global__ static void project(float *out, const float *x, const float *first, const float *second,
                               const float *third, int in, int rows, int first_rows, int second_rows)""",
       """template <class Weight>
__global__ static void project(float *out, const float *x, const Weight *first, const Weight *second,
                               const Weight *third, int in, int rows, int first_rows, int second_rows,
                               const Weight *bias = nullptr)""")
change(cu, "    const float *weight;", "    const Weight *weight;")
change(cu, """    if (in % 4 == 0 && ((uintptr_t)weight & 15u) == 0) {
        const float4 *w = reinterpret_cast<const float4 *>(weight);
        const float4 *v = reinterpret_cast<const float4 *>(x);
        for (int i = lane; i < in / 4; i += 32) {
            float4 a = w[i], b = v[i];
            sum += a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        }
    } else {
        for (int i = lane; i < in; i += 32)
            sum += weight[i] * x[i];
    }""",
       """    if (in % 4 == 0 && ((uintptr_t)weight & (4 * sizeof(Weight) - 1)) == 0) {
        const float4 *v = reinterpret_cast<const float4 *>(x);
        for (int i = lane; i < in / 4; i += 32) {
            float4 a, b = v[i];
            if constexpr (sizeof(Weight) == 2) {
                const auto *w = reinterpret_cast<const __nv_bfloat162 *>(weight);
                float2 ab = __bfloat1622float2(w[2 * i]);
                float2 cd = __bfloat1622float2(w[2 * i + 1]);
                a = make_float4(ab.x, ab.y, cd.x, cd.y);
            } else {
                a = reinterpret_cast<const float4 *>(weight)[i];
            }
            sum += a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        }
    } else {
        for (int i = lane; i < in; i += 32)
            sum += weight_value(weight[i]) * x[i];
    }""")
change(cu, "        out[row] = sum;", "        out[row] = sum + (bias ? weight_value(bias[row]) : 0.0f);")
change(cu, """static void enqueue_forward(NvCuda *g) {
    NvConfig c = g->cfg;
    NvWeights w = g->weights;""",
       """template <class Weight>
static void enqueue_forward(NvCuda *g, const Weight *bias) {
    NvConfig c = g->cfg;
    NvWeights w = g->weights;
    auto weight = [](const float *p) { return reinterpret_cast<const Weight *>(p); };""")
for old, new in (
    ("w.embedding, g->input", "weight(w.embedding), g->input"),
    ("w.att_norm + (size_t)l * d, d", "weight(w.att_norm) + (size_t)l * d, d"),
    ("w.wq + l * dd, w.wk + l * kd, w.wv + l * kd, d, d + 2 * k, d, k);",
     "weight(w.wq) + l * dd, weight(w.wk) + l * kd, weight(w.wv) + l * kd,\n"
     "            d, d + 2 * k, d, k, bias ? bias + (size_t)l * (d + 2 * k) : nullptr);"),
    ("w.wo + l * dd, nullptr", "weight(w.wo) + l * dd, nullptr"),
    ("w.ffn_norm + (size_t)l * d, d", "weight(w.ffn_norm) + (size_t)l * d, d"),
    ("w.w1 + l * hd, w.w3 + l * hd, nullptr", "weight(w.w1) + l * hd, weight(w.w3) + l * hd, nullptr"),
    ("w.w2 + l * hd, nullptr", "weight(w.w2) + l * hd, nullptr"),
    ("w.final_norm, d", "weight(w.final_norm), d"),
    ("w.classifier, nullptr", "weight(w.classifier), nullptr"),
):
    change(cu, old, new)
path = root / cu
source = path.read_text(encoding="utf-8")
assert source.count("project<<<") == 5
path.write_text(source.replace("project<<<", "project<Weight><<<"), encoding="utf-8")
change(cu, "float angle = (float)pos / powf(10000.0f, (float)i / (float)g->head_size);",
       """float theta = model->qwen_bf16_bias ? 1000000.0f : 10000.0f;
            float angle = (float)pos / powf(theta, (float)i / (float)g->head_size);""")
change(cu, "    enqueue_forward(g);",
       """    if (model->qwen_bf16_bias)
        enqueue_forward<__nv_bfloat16>(g, reinterpret_cast<const __nv_bfloat16 *>(mapped(model->qwen_bf16_bias)));
    else
        enqueue_forward<float>(g, nullptr);""")

worker = "tests/nanovllm/remote_worker.c"
change(worker, "#include <inttypes.h>", "#include <inttypes.h>\n#include <fcntl.h>\n#include <sys/mman.h>\n#include <sys/stat.h>\n#include <unistd.h>")
change(worker, "int main(int argc, char **argv)", r'''/* The compact checkpoint is used only by the CUDA benchmark runner. */
static float *qwen_weights(unsigned char **at, size_t n) {
    float *p = (float *)*at;
    *at += n * sizeof(uint16_t);
    return p;
}

static void load_qwen_eval(NvModel *model, const char *path) {
    const NvConfig expected = {2048, 11008, 36, 16, 2, 151936, 2048};
    size_t d = expected.dim, h = expected.hidden_dim, l = expected.n_layers;
    size_t k = d / (size_t)expected.n_heads * (size_t)expected.n_kv_heads;
    size_t weights = (size_t)expected.vocab_size * d + d +
        l * (2 * d + 2 * d * d + 2 * k * d + 3 * d * h + d + 2 * k);
    size_t size = sizeof(expected) + weights * sizeof(uint16_t);
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size != (off_t)size)
        nv_fatal("invalid evaluation Qwen checkpoint size");
    unsigned char *mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED || memcmp(mapping, &expected, sizeof(expected)))
        nv_fatal("invalid evaluation Qwen checkpoint header");
    *model = (NvModel){.config = expected, .mapping = mapping, .mapping_size = size};
    unsigned char *at = mapping + sizeof(expected);
    NvWeights *w = &model->weights;
    w->embedding = qwen_weights(&at, (size_t)expected.vocab_size * d);
    w->att_norm = qwen_weights(&at, l * d);
    w->wq = qwen_weights(&at, l * d * d);
    w->wk = qwen_weights(&at, l * k * d);
    w->wv = qwen_weights(&at, l * k * d);
    w->wo = qwen_weights(&at, l * d * d);
    w->ffn_norm = qwen_weights(&at, l * d);
    w->w1 = qwen_weights(&at, l * d * h);
    w->w2 = qwen_weights(&at, l * h * d);
    w->w3 = qwen_weights(&at, l * d * h);
    w->final_norm = qwen_weights(&at, d);
    w->classifier = w->embedding;
    model->qwen_bf16_bias = qwen_weights(&at, l * (d + 2 * k));
    if ((size_t)(at - mapping) != size)
        nv_fatal("invalid evaluation Qwen tensor layout");
}

int main(int argc, char **argv)''')
change(worker, "    nv_model_load(&model, argv[1]);",
       """    if (getenv("KVF_EVAL_QWEN")) {
        if (!use_cuda)
            nv_fatal("evaluation Qwen checkpoint requires CUDA");
        load_qwen_eval(&model, argv[1]);
    } else {
        nv_model_load(&model, argv[1]);
    }""")


```

<!-- bench-file: pack-qwen.py -->
```python
import json
from pathlib import Path
import struct
import sys

import numpy as np
from tokenizers import Tokenizer

source, destination = map(Path, sys.argv[1:3])
config = json.loads((source / "config.json").read_text())
index = json.loads((source / "model.safetensors.index.json").read_text())["weight_map"]
keys = ("hidden_size", "intermediate_size", "num_hidden_layers", "num_attention_heads",
        "num_key_value_heads", "vocab_size")
d, h, layers, heads, kv_heads, vocab = [config[k] for k in keys]
assert (d, h, layers, heads, kv_heads, vocab) == (2048, 11008, 36, 16, 2, 151936)
assert config["model_type"] == "qwen2" and config["rope_theta"] == 1e6
assert config["rms_norm_eps"] == 1e-6 and config["tie_word_embeddings"]
assert config["hidden_act"] == "silu" and not config["use_sliding_window"]
head_dim = d // heads
k = head_dim * kv_heads
headers = {}
for name in set(index.values()):
    with (source / name).open("rb") as f:
        length, = struct.unpack("<Q", f.read(8))
        headers[name] = (8 + length, json.loads(f.read(length)))
used = set()

def weight(name, shape, rotary_heads=0):
    shard = index[name]
    base, header = headers[shard]
    entry = header[name]
    assert entry["dtype"] == "BF16" and tuple(entry["shape"]) == shape, (name, entry)
    used.add(name)
    value = np.memmap(source / shard, mode="r", dtype="<u2",
                      offset=base + entry["data_offsets"][0], shape=shape)
    if rotary_heads:
        # Qwen rotates two half-head vectors. Nano rotates adjacent pairs.
        value = value.reshape(rotary_heads, 2, head_dim // 2, -1).transpose(0, 2, 1, 3)
    return value

output = destination / "qwen-eval-bf16.bin"
with output.open("wb") as f:
    f.write(struct.pack("<7i", d, h, layers, heads, kv_heads, vocab, 2048))
    weight("model.embed_tokens.weight", (vocab, d)).tofile(f)
    for name, shape, n_heads in (
        ("input_layernorm.weight", (d,), 0),
        ("self_attn.q_proj.weight", (d, d), heads),
        ("self_attn.k_proj.weight", (k, d), kv_heads),
        ("self_attn.v_proj.weight", (k, d), 0),
        ("self_attn.o_proj.weight", (d, d), 0),
        ("post_attention_layernorm.weight", (d,), 0),
        ("mlp.gate_proj.weight", (h, d), 0),
        ("mlp.down_proj.weight", (d, h), 0),
        ("mlp.up_proj.weight", (h, d), 0),
    ):
        for layer in range(layers):
            weight(f"model.layers.{layer}.{name}", shape, n_heads).tofile(f)
    weight("model.norm.weight", (d,)).tofile(f)
    for layer in range(layers):
        for proj, shape, n_heads in (("q", (d,), heads), ("k", (k,), kv_heads), ("v", (k,), 0)):
            weight(f"model.layers.{layer}.self_attn.{proj}_proj.bias", shape, n_heads).tofile(f)
assert used == set(index), set(index) - used
print(f"Qwen BF16 checkpoint {output.stat().st_size} bytes, parameters {(output.stat().st_size-28)//2}", flush=True)
assert output.stat().st_size == 28 + 6171877376

# The benchmark submits official tokenizer IDs. This table is used for the native cache namespace.
tok = Tokenizer.from_file(str(source / "tokenizer.json"))
by_id = {i: piece for piece, i in tok.get_vocab().items()}
assert max(by_id) < vocab and len(set(by_id)) == len(tok.get_vocab())
pieces = [by_id.get(i, f"<qwen_reserved_{i}>").encode("utf-8") for i in range(vocab)]
space = tok.token_to_id("Ġ")
assert space is not None and b"\x00" not in b"".join(pieces)
pieces[space] = b" "
with (destination / "qwen-eval-vocab.bin").open("wb") as f:
    f.write(struct.pack("<i", max(map(len, pieces))))
    for piece in pieces:
        f.write(struct.pack("<fi", 0.0, len(piece)))
        f.write(piece)


```

<!-- bench-file: run-eval.py -->
```python
import argparse
import contextlib
import csv
import json
import math
import os
from pathlib import Path
import sys

from tokenizers import Tokenizer

p = argparse.ArgumentParser()
p.add_argument("--work", type=Path, required=True)
p.add_argument("--assets", type=Path, required=True)
p.add_argument("--out", type=Path, required=True)
p.add_argument("--experiment", choices=("a", "b"), required=True)
p.add_argument("--rounds", type=int, required=True)
p.add_argument("--shard", type=int, default=10)
p.add_argument("--legacy-rounds", type=int, default=0)
args = p.parse_args()
assert not args.legacy_rounds or args.legacy_rounds <= args.rounds
assert args.rounds > 0 and args.shard > 0
work = args.work.resolve()
bindir = work / "bench-cuda/bin"
sys.path.insert(0, str(work / "bench-source/tests/nanovllm"))
from test_remote_inference import Child

args.out.mkdir(parents=True, exist_ok=False)
tokenizer = Tokenizer.from_file(str(args.assets / "tokenizer.json"))
page = 32
layers = 36
bytes_per_token = 2 * layers * 2 * 128 * 4
batch_pages = min(8, (8 << 20) // (page * bytes_per_token))
modes = ("cold", "local", "remote")


def record(file, rows):
    rows = list(rows)
    if not rows:
        return
    with file.open("a", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        if stream.tell() == 0:
            writer.writeheader()
        writer.writerows(rows)


def ids(items):
    return " ".join(map(str, items))


def block(text, length):
    tokens = tokenizer.encode(text, add_special_tokens=False).ids
    return (tokens * math.ceil(length / len(tokens)))[:length]


def directory():
    return Child([bindir / "kvfabricd", "--listen", "127.0.0.1:0"], directory=True)


def worker(mode, new, control="-", namespace="resume", legacy=False, blocks=48):
    env = dict(os.environ, OPENBLAS_NUM_THREADS="1", OMP_NUM_THREADS="1", KVF_EVAL_QWEN="1",
               NV_TEST_KV_TRANSPORT="tcp", NV_TEST_PUBLISH_GENERATED="1")
    env.pop("KVF_RESUME_EVAL_LEGACY", None)
    if legacy:
        env["KVF_RESUME_EVAL_LEGACY"] = "1"
    return Child([bindir / "test_nano_remote_worker", work / "qwen-eval-bf16.bin",
                  work / "qwen-eval-vocab.bin", "cuda", mode, control, "127.0.0.1:0", "-",
                  f"eval-{namespace}-{os.getpid()}-{worker.count()}", namespace, page, blocks, blocks,
                  12000, new], env=env)


from itertools import count
worker.count = count().__next__


def request(child, inputs, hashes=False):
    return child.infer(*(ids(x) for x in inputs), command="eval-hash" if hashes else "eval-ids")


def reset(child):
    assert child.infer(command="eval-reset") == {"reset": True}


def warm(child):
    request(child, [block("Unrelated warmup. Compare two different patterns for planning daily events.", 16)])


def check_accounting(result, mode, share, new, *, strict=True):
    items = result["items"]
    offered = len(items)
    assert all(len(item["tokens"]) == new and 0 < item["prefill_ns"] <= item["ttft_ns"] <= result["batch_ns"]
               for item in items), result
    assert [item["prompt_tokens"] for item in items] and sum(x["prompt_tokens"] for x in items) == (
        result["prefilled"] + result["local_tokens"] + result["remote_tokens"]), result
    assert sum(item["reused_tokens"] for item in items) == result["remote_tokens"], result
    if mode in ("remote", "legacy"):
        assert result["local_tokens"] == 0 and result["transport"] == "TCP", result
        for item in items:
            reused = item["reused_tokens"]
            assert 0 <= reused <= share and reused % page == 0, result
            if strict:
                assert reused == share, result
            if reused:
                assert item["received_bytes"] >= reused * bytes_per_token, result
        if strict:
            assert result["prefetch_failures"] == result["fallbacks"] == result["admission_skips"] == 0, result
            calls = share // page * layers if mode == "legacy" else math.ceil(share // page / batch_pages)
            assert all(item["received_bytes"] == share * bytes_per_token and item["data_requests"] == calls
                       for item in items), result
    else:
        assert result["remote_tokens"] == 0 and result["local_tokens"] == (
            share * offered if mode == "local" else 0), result


def wave_row(result, **labels):
    return {
        **labels,
        "requests": len(result["items"]),
        "input_tokens": sum(item["prompt_tokens"] for item in result["items"]),
        "output_tokens": sum(len(item["tokens"]) for item in result["items"]),
        "batch_ns": result["batch_ns"],
        "batch_prefill_ns": max(item["prefill_ns"] for item in result["items"]),
        "actual_prefilled": result["prefilled"],
        "local_reused": result["local_tokens"],
        "remote_reused": result["remote_tokens"],
        "remote_hit_requests": sum(item["reused_tokens"] > 0 for item in result["items"]),
        "prefetch_requests": result["prefetch_requests"],
        "prefetch_failures": result["prefetch_failures"],
        "fallbacks": result["fallbacks"],
        "admission_skips": result["admission_skips"],
        "transport": result["transport"],
    }


def request_rows(result, **labels):
    return [
        {**labels, "request_index": i + 1, **{k: v for k, v in t.items() if k not in ("tokens", "kv_page_hashes")},
         "generated_out": ids(t["tokens"]),
         "prompt_kv_sha256": " ".join(t.get("kv_page_hashes", ()))}
        for i, t in enumerate(result["items"])
    ]


def ttft():
    prefix = block("<|im_start|>system\nYou are an assistant analyzing an engineering log.<|im_end|>\n"
                   "<|im_start|>user\nDuring the night shift, the team recorded requests, service times,"
                   " checksums, process restarts, and the ordering of disk operations. ", 448)
    targets = [
        prefix + block(f"Follow-up {r}: State what the team should verify before treating a cached page as valid."
                       "<|im_end|>\n<|im_start|>assistant\n", 64)
        for r in range(1, args.rounds + 1)
    ]
    assert len({tuple(t[448:480]) for t in targets}) == len(targets)
    record(args.out / "a-input.csv", ({"round": i, "prompt_ids": ids(v)} for i, v in enumerate(targets, 1)))
    observed = {}
    with directory() as ctrl, worker("remote", 8, ctrl.endpoint, namespace="long") as publisher:
        warm(publisher)
        pub = request(publisher, [prefix])
        assert pub["published"] >= len(prefix) // page, pub
        for shard_index, first in enumerate(range(0, args.rounds, args.shard)):
            order = modes[shard_index % 3:] + modes[:shard_index % 3]
            for mode in order:
                with worker(mode, 8, ctrl.endpoint, namespace="long") as consumer:
                    warm(consumer)
                    if mode == "local":
                        request(consumer, [prefix])
                    print(f"A {mode} ready, rounds {first+1} to {min(first+args.shard, args.rounds)}", flush=True)
                    for r in range(first, min(first + args.shard, args.rounds)):
                        if mode == "remote":
                            reset(consumer)
                        result = request(consumer, [targets[r]], hashes=True)
                        labels = dict(round=r + 1, mode=mode, page_tokens=page, offered_shared=448)
                        item, = request_rows(result, **labels)
                        record(args.out / "a-requests.csv", [{**item, **wave_row(result)}])
                        check_accounting(result, mode, 448, 8)
                        assert len(item["prompt_kv_sha256"].split()) == 16, item
                        observed[(r, mode)] = (item["generated_out"], item["prompt_kv_sha256"])
                        print(f"A {mode} {r+1}: TTFT {item['ttft_ns']/1e6:.1f} ms, "
                              f"reuse {item['reused_tokens']}, RPC {item['data_requests']}", flush=True)
        if args.legacy_rounds:
            with worker("remote", 8, ctrl.endpoint, namespace="long", legacy=True) as consumer:
                warm(consumer)
                for r in range(args.legacy_rounds):
                    reset(consumer)
                    result = request(consumer, [targets[r]], hashes=True)
                    labels = dict(round=r + 1, mode="legacy", page_tokens=page, offered_shared=448)
                    item, = request_rows(result, **labels)
                    record(args.out / "a-requests.csv", [{**item, **wave_row(result)}])
                    check_accounting(result, "legacy", 448, 8)
                    observed[(r, "legacy")] = (item["generated_out"], item["prompt_kv_sha256"])
                    print(f"A legacy {r+1}: {item['data_requests']} data RPC", flush=True)
    for (r, mode), actual in observed.items():
        assert actual == observed[(r, "cold")], (r, mode, actual[0], observed[(r, "cold")][0])
    print(f"A validated {len(observed)} rows against Cold, including all full K/V pages.", flush=True)


def follow_up(turn, variant, session):
    return block(f"<|im_end|>\n<|im_start|>user\nCase {variant} in session {session}, follow-up {turn}. "
                 "Summarize the previous finding and state the next verification step."
                 "<|im_end|>\n<|im_start|>assistant\n", 28)


def throughputs():
    t1 = [block(f"<|im_start|>user\nAnalyze {topic} for the service. What is the first item to verify?"
                "<|im_end|>\n<|im_start|>assistant\n", 32)
          for topic in ("availability", "latency", "memory capacity", "disk durability")]
    assert len({tuple(x[:page]) for x in t1}) == 4
    with worker("cold", 4, namespace="history") as oracle:
        first = request(oracle, t1)
        t1_history = [tokens + item["tokens"] for tokens, item in zip(t1, first["items"])]
        t2 = [tokens + follow_up(2, 1, i) for i, tokens in enumerate(t1_history)]
        second = request(oracle, t2)
        t2_history = [tokens + item["tokens"] for tokens, item in zip(t2, second["items"])]
    observed = {}
    with directory() as ctrl:
        for turn, previous, history in ((2, t1, t1_history), (3, t2, t2_history)):
            space = f"chat-{turn}"
            share = len(previous[0])
            with worker("remote", 4, ctrl.endpoint, namespace=space) as publisher:
                warm(publisher)
                assert request(publisher, previous)["published"] >= 4 * share // page
                for concurrency in (1, 2, 4):
                    targets = [
                        [history[i] + follow_up(turn, r, i) for i in range(concurrency)]
                        for r in range(1, args.rounds + 1)
                    ]
                    for i in range(concurrency):
                        assert len({tuple(v[i][share:share+page]) for v in targets}) == args.rounds
                    record(args.out / "b-input.csv", (
                        dict(turn=turn, concurrency=concurrency, round=r + 1, request_index=i + 1,
                             prompt_ids=ids(prompt))
                        for r, wave in enumerate(targets) for i, prompt in enumerate(wave)))
                    first_mode = (turn + concurrency) % len(modes)
                    order = modes[first_mode:] + modes[:first_mode]
                    for mode in order:
                        with worker(mode, 4, ctrl.endpoint, namespace=space) as consumer:
                            warm(consumer)
                            if mode == "local":
                                request(consumer, previous[:concurrency])
                            print(f"B turn={turn} concurrency={concurrency} mode={mode}, {args.rounds} waves", flush=True)
                            for r, wave in enumerate(targets, 1):
                                if mode == "remote":
                                    reset(consumer)
                                result = request(consumer, wave)
                                labels = dict(turn=turn, concurrency=concurrency, round=r, mode=mode,
                                              page_tokens=page, offered_shared=share)
                                record(args.out / "b-waves.csv", [wave_row(result, **labels)])
                                record(args.out / "b-requests.csv", request_rows(result, **labels))
                                check_accounting(result, mode, share, 4, strict=False)
                                observed[(turn, concurrency, r, mode)] = [item["tokens"] for item in result["items"]]
                                if r == 1 or r == args.rounds or r % 4 == 0:
                                    print(f"B {mode} wave {r}: {result['batch_ns']/1e6:.1f} ms, "
                                          f"reuse {result['remote_tokens']}, fallbacks {result['fallbacks']}", flush=True)
    for (turn, concurrency, r, mode), seq in observed.items():
        assert seq == observed[(turn, concurrency, r, "cold")], (turn, concurrency, r, mode)
    print(f"B validated all generation IDs across {len(observed)} waves.", flush=True)


if args.experiment == "a":
    ttft()
else:
    throughputs()


```

<!-- bench-file: run-ssd.sh -->
```bash
#!/usr/bin/env bash
set -euo pipefail
work=$(realpath "$1")
test "$(findmnt -n -o FSTYPE --target "$work")" = ext4
common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DMINIINFER_WITH_RDMA=OFF
        -DMINIINFER_BUILD_NANOVLLM=OFF -DMINIINFER_BUILD_INTEGRATION=OFF
        -DKVF_BUILD_TESTS=OFF -DMINIINFER_BUILD_TESTS=OFF)
for mode in OFF ON; do
    cmake -S "$work/bench-source" -B "$work/ssd-$mode" "${common[@]}" -DMINIINFER_WITH_URING="$mode"
    cmake --build "$work/ssd-$mode" --target kvf_bench -j 6
done
output="$work/raw/c"
mkdir "$output"
scratch=$(mktemp -d "$work/ssd-files.XXXXXX")
rmdir_on_exit() { rmdir "$scratch"; }
trap rmdir_on_exit EXIT
uid=$(id -u)
gid=$(id -g)
test "$uid" -ne 0
raise_limit=(prlimit --memlock=268435456:268435456 --)
hard_limit=$(ulimit -Hl)
if [[ $hard_limit == unlimited ]] || ((hard_limit >= 262144)); then
    launch=("${raise_limit[@]}")
elif [[ -n ${WSL_DISTRO_NAME-} && -x /mnt/c/Windows/System32/wsl.exe ]]; then
    launch=(/mnt/c/Windows/System32/wsl.exe -d "$WSL_DISTRO_NAME" --user root --
            "${raise_limit[@]}" setpriv --reuid "$uid" --regid "$gid" --clear-groups)
else
    launch=(sudo "${raise_limit[@]}" setpriv --reuid "$uid" --regid "$gid" --clear-groups)
fi
printf '%s\n' 'mode,device,layer_bytes,concurrency,round,lane,sample,latency_ns,write_ns,read_ns' >"$output/c-requests.csv"
printf '%s\n' 'mode,device,layer_bytes,concurrency,round,samples,wall_ns,throughput_mib_s,p50_us,p95_us,p99_us,client_cpu_ms,directory_cpu_ms,source_cpu_ms,client_peak_rss_kib,directory_peak_rss_kib,source_peak_rss_kib' >"$output/c-rounds.csv"
for setup in 4:OFF 4:ON 32:ON 32:OFF; do
    IFS=: read -r concurrency mode <<<"$setup"
    label=direct-posix
    if [[ $mode == ON ]]; then label=direct-uring-fixed; fi
    logfile="$work/c-$concurrency-$mode.log"
    "${launch[@]}" "$work/ssd-$mode/bin/kvf_bench" ssd direct "$scratch" 4194304 "$concurrency" \
        "$output/c-requests.csv" "$output/c-rounds.csv" 2>&1 | tee "$logfile"
    grep -q "mode=$label" "$logfile"
done

```

<!-- bench-file: summarize.py -->
```python
import csv
import math
from collections import defaultdict
from pathlib import Path
from statistics import median
import sys

root = Path(sys.argv[1])


def read(file):
    with (root / file).open(encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def write(file, rows):
    with (root / file).open("w", encoding="utf-8", newline="") as stream:
        csv_out = csv.DictWriter(stream, list(rows[0]))
        csv_out.writeheader()
        csv_out.writerows(rows)


def q(values, percent):
    values = sorted(values)
    return values[math.ceil(len(values) * percent / 100) - 1]


def latency(rows, field, prefix):
    values = [int(r[field]) / 1e6 for r in rows]
    return {f"{prefix}_p{p}_ms": f"{q(values, p):.6f}" for p in (50, 95, 99)}


def total(rows, key):
    return sum(int(r[key]) for r in rows)


def profile_a():
    rows = read("a-requests.csv")
    groups = defaultdict(list)
    keys = {(r["round"], r["mode"]) for r in rows}
    assert len(keys) == len(rows)
    cold = {r["round"]: r for r in rows if r["mode"] == "cold"}
    for r in rows:
        groups[r["mode"]].append(r)
        assert r["generated_out"] == cold[r["round"]]["generated_out"]
        assert r["prompt_kv_sha256"] == cold[r["round"]]["prompt_kv_sha256"]
        assert len(r["prompt_kv_sha256"].split()) == 16
        assert int(r["prompt_tokens"]) == 512 and len(r["generated_out"].split()) == 8
        assert int(r["actual_prefilled"]) == (512 if r["mode"] == "cold" else 64)
        if r["mode"] in ("remote", "legacy"):
            assert (int(r["local_reused"]), int(r["reused_tokens"]), int(r["received_bytes"])) == (0, 448, 33030144)
            assert int(r["data_requests"]) == (5 if r["mode"] == "remote" else 504)
            assert int(r["fallbacks"]) == int(r["prefetch_failures"]) == 0
    result = []
    for mode in ("cold", "local", "remote", "legacy"):
        if mode not in groups:
            continue
        group = groups[mode]
        selected_cold = [cold[r["round"]] for r in group]
        p50 = q([int(r["ttft_ns"]) for r in group], 50)
        cold_p50 = q([int(r["ttft_ns"]) for r in selected_cold], 50)
        result.append(dict(mode=mode, requests=len(group), prompt_tokens=512, page_tokens=32, offered_shared=448,
                           generated_tokens=8, matching_generated_ids=len(group), matching_full_kv_pages=len(group) * 16,
                           prefill_tokens=group[0]["actual_prefilled"], remote_reused_tokens=group[0]["reused_tokens"],
                           local_reused_tokens=group[0]["local_reused"], rpc_per_request=group[0]["data_requests"],
                           bytes_per_request=group[0]["received_bytes"], fallbacks=total(group, "fallbacks"),
                           fallback_pct=100*total(group, "fallbacks")/len(group),
                           **latency(group, "ttft_ns", "ttft"), **latency(group, "net_recv_ns", "net_recv"),
                           network_share_of_ttft_pct_p50=f'{q([100*int(r["net_recv_ns"])/int(r["ttft_ns"]) for r in group],50):.6f}',
                           p50_decrease_pct_vs_matched_cold=f"{100*(1-p50/cold_p50):.6f}",
                           faster_than_matched_cold=sum(int(r["ttft_ns"]) < int(cold[r["round"]]["ttft_ns"]) for r in group)))
    write("a-summary.csv", result)


def profile_b():
    waves = read("b-waves.csv")
    reqs = read("b-requests.csv")
    req_keys = ("turn", "concurrency", "round", "request_index")
    cold = {tuple(r[k] for k in req_keys): r["generated_out"] for r in reqs if r["mode"] == "cold"}
    assert len(cold) * 3 == len(reqs)
    for r in reqs:
        assert r["generated_out"] == cold[tuple(r[k] for k in req_keys)]
        assert int(r["prompt_tokens"]) == (64 if r["turn"] == "2" else 96)
        assert len(r["generated_out"].split()) == 4
    result = []
    for concurrency in (1, 2, 4):
        common = [r for r in waves if int(r["concurrency"]) == concurrency]
        cold_group = [r for r in common if r["mode"] == "cold"]
        cold_rate = total(cold_group, "requests") * 1e9 / total(cold_group, "batch_ns")
        for mode in ("cold", "local", "remote"):
            group = [r for r in common if r["mode"] == mode]
            requests = [r for r in reqs if int(r["concurrency"]) == concurrency and r["mode"] == mode]
            count = total(group, "requests")
            runtime = total(group, "batch_ns") / 1e9
            prefill = total(group, "batch_prefill_ns") / 1e9
            output = total(group, "output_tokens")
            inputs = total(group, "input_tokens")
            actual = total(group, "actual_prefilled")
            assert count == len(requests) and runtime > 0
            assert inputs == actual + total(group, "local_reused") + total(group, "remote_reused")
            result.append(dict(concurrency=concurrency, mode=mode, waves=len(group), requests=count,
                               total_input_tokens=inputs, output_tokens=output, matching_generated_ids=count,
                               actual_prefill_tokens=actual, reused_remote_tokens=total(group, "remote_reused"),
                               reused_local_tokens=total(group, "local_reused"), hit_requests=total(group, "remote_hit_requests"),
                               remote_hit_pct=100*total(group, "remote_hit_requests")/count,
                               fallbacks=total(group, "fallbacks"), fallback_pct=100*total(group, "fallbacks")/count,
                               prefetch_failures=total(group, "prefetch_failures"),
                               admission_skips=total(group, "admission_skips"),
                               requests_per_s=f"{count/runtime:.6f}", output_tokens_per_s=f"{output/runtime:.6f}",
                               total_tokens_per_s=f"{(inputs+output)/runtime:.6f}",
                               effective_prefill_tokens_per_s=f"{inputs/prefill:.6f}",
                               actual_prefill_tokens_per_s=f"{actual/prefill:.6f}",
                               gain_pct_req_per_s_vs_cold=f"{100*((count/runtime)/cold_rate-1):.6f}",
                               **latency(requests, "ttft_ns", "ttft")))
    write("b-summary.csv", result)


def profile_c():
    rows, rounds = read("c-requests.csv"), read("c-rounds.csv")
    assert {r["mode"] for r in rows} == {"direct-posix", "direct-uring-fixed"}
    result = []
    for concurrency in (4, 32):
        common = [r for r in rounds if int(r["concurrency"]) == concurrency]
        baseline = median(float(r["throughput_mib_s"]) for r in common if r["mode"] == "direct-posix")
        for mode in ("direct-posix", "direct-uring-fixed"):
            group = [r for r in common if r["mode"] == mode]
            raw = [r for r in rows if int(r["concurrency"]) == concurrency and r["mode"] == mode]
            assert len(group) == 3 and len(raw) == concurrency * 16 * len(group)
            assert all(int(r["write_ns"]) > 0 and int(r["read_ns"]) > 0 and
                       int(r["write_ns"]) + int(r["read_ns"]) <= int(r["latency_ns"]) for r in raw)
            rate = median(float(r["throughput_mib_s"]) for r in group)
            result.append(dict(concurrency=concurrency, mode=mode, payload_mib=4, rounds=len(group), samples=len(raw),
                               total_mib_per_s=rate, pct_change_vs_direct_posix=100*(rate/baseline-1),
                               write_mib_per_s_per_op=4e9*len(raw)/total(raw, "write_ns"),
                               read_mib_per_s_per_op=4e9*len(raw)/total(raw, "read_ns"),
                               **latency(raw, "latency_ns", "request"), **latency(raw, "write_ns", "write"),
                               **latency(raw, "read_ns", "read")))
    write("c-summary.csv", result)


for file, action in (("a-requests.csv", profile_a), ("b-waves.csv", profile_b), ("c-requests.csv", profile_c)):
    if (root / file).exists():
        action()


```

<!-- bench-file: verify-qwen.py -->
```python
import csv
from pathlib import Path
import sys

import torch
from transformers import AutoModelForCausalLM
from transformers.utils import logging

assets, samples, dest = map(Path, sys.argv[1:])
logging.set_verbosity_error()
torch.backends.cuda.matmul.allow_tf32 = False
torch.set_float32_matmul_precision("highest")
native = {row["round"]: row["generated_out"] for row in csv.DictReader((samples / "a-requests.csv").open())
          if row["mode"] == "cold"}
prompts = list(csv.DictReader((samples / "a-input.csv").open()))[:3]
model = AutoModelForCausalLM.from_pretrained(
    assets, local_files_only=True, torch_dtype=torch.float32,
    device_map={"": 0}, attn_implementation="eager").eval()
rows = []
with torch.inference_mode():
    for row in prompts:
        inputs = torch.tensor([[int(v) for v in row["prompt_ids"].split()]], device="cuda")
        cache = None
        generated = []
        for _ in range(8):
            out = model(input_ids=inputs, past_key_values=cache, use_cache=True, logits_to_keep=1)
            cache = out.past_key_values
            inputs = out.logits[:, -1, :].argmax(dim=-1, keepdim=True)
            generated.append(inputs.item())
        text = " ".join(map(str, generated))
        rows.append(dict(round=row["round"], prompt_tokens=512, new_tokens=8,
                         hf_fp32_ids=text, nano_eval_ids=native[row["round"]],
                         match=int(text == native[row["round"]])))
        print(rows[-1], flush=True)
with dest.open("w", encoding="utf-8", newline="") as f:
    wr = csv.DictWriter(f, fieldnames=list(rows[0]))
    wr.writeheader()
    wr.writerows(rows)
assert all(r["match"] for r in rows), rows


```

<!-- bench-file: metadata.py -->
```python
import csv
from hashlib import sha256
import json
from importlib.metadata import version
from datetime import date
from pathlib import Path
import re
import subprocess
import sys

work, assets, out = map(Path, sys.argv[1:])
def save(name, rows):
    rows = list(rows)
    with (out / name).open("w", encoding="utf-8", newline="") as f:
        wr = csv.DictWriter(f, list(rows[0]))
        wr.writeheader()
        wr.writerows(rows)

def digest(file):
    sha = sha256()
    with file.open("rb") as f:
        for block in iter(lambda: f.read(8 << 20), b""):
            sha.update(block)
    return sha.hexdigest()

config = json.loads((assets / "config.json").read_text())
checkpoint = work / "qwen-eval-bf16.bin"
model_files = [assets / name for name in ("config.json", "tokenizer.json", "model.safetensors.index.json",
               "model-00001-of-00002.safetensors", "model-00002-of-00002.safetensors")]
model_files += [checkpoint, work / "qwen-eval-vocab.bin"]
save("model-provenance.csv", (
    dict(model="Qwen2.5-3B-Instruct", file=p.name, bytes=p.stat().st_size, sha256=digest(p),
         parameter_count=(checkpoint.stat().st_size-28)//2, source_weight_dtype=config["torch_dtype"],
         kv_dtype="FP32", official_context=config["max_position_embeddings"], adapter_context=2048,
         layers=config["num_hidden_layers"], query_heads=config["num_attention_heads"],
         kv_heads=config["num_key_value_heads"], head_dim=config["hidden_size"] // config["num_attention_heads"])
    for p in model_files))

def cmd(*args):
    return subprocess.check_output(args, text=True).strip()

device = cmd("findmnt", "-n", "-o", "SOURCE", "--target", str(work))
save("environment.csv", [dict(item=key, value=value) for key, value in (
    ("measurement_date", date.today().isoformat()),
    ("kernel", cmd("uname","-r")),
    ("python", sys.version.splitlines()[0]),
    ("numpy", version("numpy")),
    ("tokenizers", version("tokenizers")),
    ("distro", cmd("lsb_release","-ds")),
    ("cpu", next(line.split(":",1)[1].strip() for line in cmd("lscpu").splitlines() if line.startswith("Model name:"))),
    ("gpu", cmd("nvidia-smi","--query-gpu=name,memory.total,driver_version","--format=csv,noheader")),
    ("gpu_before", (work / "gpu-before.csv").read_text().splitlines()[1]),
    ("gpu_after", (work / "gpu-after.csv").read_text().splitlines()[1]),
    ("scratch_device", cmd("findmnt","-n","-o","SOURCE,FSTYPE","--target",str(work))),
    ("scratch_device_model", cmd("lsblk", "-dno", "MODEL", device) if Path(device).is_block_device() else ""),
    ("repository_mount", cmd("findmnt", "-n", "-o", "SOURCE,FSTYPE", "--target", str(Path.cwd()))),
    ("ssd_payload_bytes", "4194304"),
    ("cpu_toolchain", cmd("gcc","--version").splitlines()[0]),
    ("sanitizer_compiler", cmd("clang","--version").splitlines()[0]),
    ("cuda", cmd("/usr/local/cuda-12.8/bin/nvcc","--version").splitlines()[-2]),
    ("cmake", cmd("cmake","--version").splitlines()[0]),
    ("liburing", cmd("pkg-config","--modversion","liburing")),
    ("openssl", cmd("pkg-config","--modversion","openssl")),
    ("inference_network", "127.0.0.1 IPv4 TCP (independent directory and Worker processes, one GPU)"),
    ("wire_header_bytes", "36"),
    ("batch_limit_pages", "8"),
    ("batch_limit_bytes", "8388608"),
    ("effective_page_tokens", "32"),
    ("prefetch_deadline_ms", "12000"),
    ("ssd_process_memlock_bytes", "268435456"),
    ("git_head", "06b7f286d13c20c26e01803d7c43fb09adcd83eb"),
)])

base = work / "source"
paths = [base / "CMakeLists.txt"]
for folder in ("src", "include", "tools", "tests", "cmake", "config", "examples", "third_party"):
    paths += [p for p in (base / folder).rglob("*") if p.is_file() and p.suffix != ".pyc"]
save("source-snapshot.csv", (
    dict(relative_path=p.relative_to(base).as_posix(), bytes=p.stat().st_size, sha256=digest(p))
    for p in sorted(paths)))

checks = []
for name, count in (("release-cpu", 14), ("release-cuda", 17), ("asan-cpu", 14)):
    log = (work / f"{name}-ctest.log").read_text()
    assert f"100% tests passed, 0 tests failed out of {count}" in log
    rows = re.findall(r"^\s*\d+/\d+ Test\s+#\d+:\s+(\S+)\s+\.+\s+Passed\s+([\d.]+) sec$", log, re.M)
    assert len(rows) == count, (name, rows)
    checks.extend(dict(build=name, test=case, status="passed", seconds=duration,
                       configured_tests=count) for case, duration in rows)
save("d-ctest.csv", checks)

backends = []
for concurrency, flag in ((4, "OFF"), (4, "ON"), (32, "ON"), (32, "OFF")):
    log = (work / f"c-{concurrency}-{flag}.log").read_text()
    modes = set(re.findall(r"mode=([-a-z]+)", log))
    expected = "direct-uring-fixed" if flag == "ON" else "direct-posix"
    assert modes == {expected}, log
    backends.append(dict(concurrency=concurrency, cmake_with_uring=flag, verified_mode=expected,
                         memlock_bytes=268435456, startup=log.splitlines()[0]))
save("c-backends.csv", backends)


```
