# KV-Fabric for vLLM V1

Phase 1 targets vLLM 0.31.0 on Linux with CUDA, BF16, TP=PP=1, causal full attention,
and 16-token cache blocks. Use Qwen3-4B when its weights are available or
Qwen2.5-3B-Instruct BF16 as the fallback. The connector stages bounded windows in
pinned CPU memory. vLLM is notified after the entire asynchronous load completes.
An unsuccessful load is reported with all its destination block IDs and the
request then recomputes under vLLM's `recompute` failure policy.

Each Worker stores either CPU-backed pages or Direct I/O-backed pages for its
namespace. The disk file is private to the running Worker. The cache identity
includes the checkpoint ID or model location, revisions, tokenizer fingerprint
when local, model configuration, layer order and BF16 layout. Use immutable
checkpoint paths or pinned revisions. `checkpoint_id` can identify a checkpoint
explicitly. Worker tokenizer identities must also match. Text prompts with cache salts or adapters bypass
the connector.

## Build and verify

Run from the repository root in a Linux environment with the CUDA NVTX headers,
liburing and the vLLM runtime installed. Mount the disk directory on ext4.

```bash
cmake -S . -B build-vllm -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DMINIINFER_BUILD_VLLM_BRIDGE=ON -DMINIINFER_WITH_URING=ON
cmake --build build-vllm
ctest --test-dir build-vllm --output-on-failure

python3 tests/vllm/test_connector_roundtrip.py \
  --model /path/to/Qwen3-4B \
  --library "$PWD/build-vllm/src/integration/libkvf_vllm.so" \
  --directory "$PWD/build-vllm/bin/kvfabricd" \
  --out /mnt/nvme/kvf-roundtrip
```

Use `--fallback-model /path/to/Qwen2.5-3B-Instruct` for the local fallback. The same
test with `--bridge-only` checks directory lookup, publish, fetch, exact byte
equality and eviction in both tiers. Build with `MINIINFER_WITH_URING=OFF` to
verify the Direct POSIX path. The connector records the actual backend name and
rejects a buffered fallback in disk mode.

## Repeatable comparison

The script launches each vLLM instance and its auxiliary process on loopback.
LMCache 0.5.8 uses its MP connector. Use `--lmcache-transfer-mode engine_driven`
when CUDA IPC is unavailable and record the selected mode. The disk arm checks
L2 writes before clearing
resident L1. vLLM GPU cache is reset between measured groups. For the Native arm,
vLLM requires its prefix cache to be enabled. Every arm gets the same input IDs,
share ratios, GPU budget and generation parameters.

```bash
python3 scripts/bench_vllm_kv.py \
  --model /path/to/Qwen3-4B \
  --library "$PWD/build-vllm/src/integration/libkvf_vllm.so" \
  --directory "$PWD/build-vllm/bin/kvfabricd" \
  --disk-dir /mnt/nvme/kvf-cache --require-native-nvme \
  --lengths 4096 8192 16384 --shared 0 0.5 0.9 \
  --requests 100 --output-tokens 128 --out /mnt/nvme/kvf-results
```

`environment.json` records the GPU, effective versions, disk mount and topology.
Each mode retains its request and summary JSONL, service logs, and where
applicable KV-Fabric commit counters. TTFT and TPOT use streamed output ID
timestamps. ITL uses output chunks and reports the number of multi-token chunks.
LMCache disk attribution is emitted only when the GPU import, L1/L2 match and
successful disk-load counters reconcile. The environment check distinguishes a
WSL2 virtual disk from native NVMe. Use a larger GPU/host cache budget and a
checkpoint whose configured context includes the output budget for 32K. Run with one shared fraction at a time if cache
pressure is outside the intended workload. An undersized cache can add
synchronous publication and eviction time to TTFT.

The C bridge emits `kvfabric.lookup` and `kvfabric.fetch`. Direct I/O emits
`kvfabric.disk_io.read` and `kvfabric.disk_io.write` from its serving threads.
The Python worker emits GPU transfer and sync ranges. These ranges can be captured
with `nsys profile --trace=cuda,nvtx,osrt` around the vLLM service.

## Profile the batched CUDA path

Configure the bridge with `-DMINIINFER_VLLM_CUDA=ON` and the target GPU's
`CMAKE_CUDA_ARCHITECTURES`. The worker uses a 16-byte CUDA gather/scatter
for supported KV layouts. Set `kv_connector_extra_config.cuda_batch` to
`false` for the original copy path. The optimized worker stages a
bounded number of snapshots and publishes them in the background. Benchmark
priming waits for the requested prefix to be published before timing starts.

```bash
bash scripts/profile_kv.sh baseline build-vllm /path/to/model /tmp/kvf-before
bash scripts/profile_kv.sh optimized build-vllm /path/to/model /tmp/kvf-after
bash scripts/profile_kv.sh bench build-vllm /path/to/model /tmp/kvf-cuda
```

The Nsight Systems runs replay the same 16K requests with a shared 8K prefix.
The analyzer intersects GPU activities with the client TTFT range and
uses CUDA correlation IDs to count copies launched by the connector. Its
active-copy bandwidth excludes gaps between DMA activities and counts only
KV payload transfers. The overlap ratio divides KV DMA time concurrent with
model GPU kernels by KV DMA active time. The CUDA
microbenchmark checks bytes after every mode and direction. Its CSV rows
give mode, direction, memcpy count, payload bytes, median elapsed
milliseconds, and effective GiB/s. Nsight Compute measures kernel DRAM
and occupancy when the driver exposes its counters.

Run the same workload without a profiler to compare TTFT and exact output IDs.
Use a fresh output directory for each process. The driver checks imported tokens
and records all per-request latencies in `requests.json`.

```bash
python3 scripts/profile_vllm_kv.py recompute build-vllm /path/to/model /tmp/kvf-recompute --requests 4
python3 scripts/profile_vllm_kv.py optimized build-vllm /path/to/model /tmp/kvf-batched --requests 4 \
  --reference /tmp/kvf-recompute/requests.json
```
