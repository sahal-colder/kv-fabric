# MiniInfer C

C11 inference and storage runtime for legacy FP32 Llama checkpoints. The
`miniinfer` executable integrates the C nano-vLLM scheduler, paged KV and
sampling with CPU and optional CUDA execution. The storage library exposes
CPU, GPU and SSD backends through C APIs. CUDA inference requires the CUDA compiler.

## Targets

| Source | Output |
| --- | --- |
| [`src/nanovllm`](../src/nanovllm/) | `nanovllm` and its C inference library, with CPU and optional CUDA inference. |
| [`src/worker`](../src/worker/) | `libminiinfer_storage.a`, C headers and a storage demo. Includes CPU/CUDA pools, tiered KV, POSIX, io_uring and SPDK backends. |
| [`src/integration`](../src/integration/) | `miniinfer` CLI using the inference sources and the MiniInfer storage APIs. |

All targets build from the repository root. To build a single component, see
[CPU storage](#standalone-storage) or [nano-vLLM](nanovllm.md).

`NANO_WITH_CUDA=ON` enables the legacy optional inference kernels.
`MINIINFER_WITH_CUDA=ON` enables the Storage product's C CUDA Runtime backend.
The two switches are independent.

## Implemented subsystems

| MiniInfer C++ area | C implementation | Notes |
|---|---|---|
| CPU and CUDA memory pools | `allocator.h`, `allocator.c` | TLSF free bins, adjacent block coalescing, address indexing, and event-delayed CUDA reuse |
| Pinned host memory | `MI_MEMORY_PINNED`, `MI_MEMORY_CUDA_PINNED` | POSIX `mlock` or CUDA `cudaMallocHost` |
| CUDA Graph allocator | `graph_allocator.h`, `graph_allocator.c` | Pre-reserved scratch workspace, aligned bump allocation, capture thread/stream ownership, per-graph workspace tracking |
| CPU/GPU storage backend | `storage_backend.h`, `storage_backend.c` | Fixed-size slab slots addressed by slot ID, with direct CUDA copies |
| SSD storage backend | `storage_backend.c` | Fixed records, 64-byte identity header, CRC32C, POSIX, io_uring or SPDK bdev |
| Weight loader | `model_mapping.c` | Read-only mmap, shared by the inference CLI and C API |
| Inference | `src/nanovllm` | CPU forward and CUDA Graph replay; optional SSD spill via the integration |
| Tiered storage / block lease | `tiered_storage.h`, `tiered_storage.c` | Stable `BlockId`, metadata/version, shared READ / exclusive WRITE, explicit lease finish/abort |
| Tier migration executor | `tiered_storage.c` | Synchronous, bounded asynchronous migration worker, batch migration, budget accounting and cleanup retry |
| KV offload cache | `kv_cache.h`, `kv_cache.c` | CPU/SSD placement, LRU demotion, demand promotion and bounded asynchronous prefetch |
| Paged KV | `paged_kv.h`, `paged_kv.c` | Per-layer pages, immutable published token pages and a prefix trie |
| File I/O | `file_io.h`, `file_io.c` | EINTR-safe positional I/O and batch helpers |

## Build and test

Build from the repository root on Linux or WSL with CMake 3.18+, a C11 compiler, pthreads and OpenSSL development headers.

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
./build-release/bin/miniinfer /path/to/stories15M.bin -z /path/to/tokenizer.bin -t 0 -n 64
```

Storage regression assertions stay enabled in Debug and Release. To run the Debug suite:

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DMINIINFER_BUILD_TESTS=ON
cmake --build build-debug -j
ctest --test-dir build-debug --output-on-failure
```

### Standalone storage

Build and test MiniInfer Storage by itself from the repository root.

```bash
cmake -S . -B build-storage -DCMAKE_BUILD_TYPE=Debug \
  -DMINIINFER_WITH_KVFABRIC=OFF -DMINIINFER_BUILD_NANOVLLM=OFF -DMINIINFER_BUILD_INTEGRATION=OFF
cmake --build build-storage -j
./build-storage/bin/miniinfer_basic
ctest --test-dir build-storage --output-on-failure
```

See [build switches](#build-switches) for the independent io_uring, SPDK and CUDA options.

### KV-Fabric C SDK

The root CMake build links the storage library to the in-tree `KVFabric::kvfabric` target.

The build installs `libminiinfer_remote.a`, `miniinfer_worker` and `kvf_bench`.
A producer publishes complete pages and serves layer bytes directly to the consumer.
The consumer verifies each layer, imports it into the chosen tier, seals the page,
and adds it to its local Prefix Trie.

```text
Control plane: Worker <-- Wire V3 metadata --> kvfabricd
Data plane:    source tier -> aligned Segment -> pooled TCP -> target tier
File tier:     accelerator <-> registered Segment <-> ext4 direct file
```

Set `MINIINFER_WITH_CUDA=ON` on hosts that run a CUDA Worker. The demo defaults
to a CUDA producer and a CPU consumer. Use the same KV-Fabric revision on both hosts.
For a CPU-only run, set `KVF_PRODUCER_DEVICE=cpu`.
[Single-host and dual-host commands](kvfabric-data-plane.md)

[Source reading guide](READING-GUIDE.md) ·
[Implementation and measured results](KV-FABRIC-IMPLEMENTATION-REPORT.md)

### Build switches

| CMake option | Values and behavior |
|---|---|
| `MINIINFER_NANO_POOL` | `MINIINFER` (default) uses the preallocated host pool for CPU KV. `NANO` uses nano-vLLM's original contiguous arrays. Both keep the same paging and prefix policy. |
| `MINIINFER_WITH_CUDA` | Builds the CUDA C Runtime transfer backend and the `cuda[:ID]` Worker device. |
| `NANO_WITH_CUDA` | Enables the existing optional CUDA inference kernels for nano-vLLM and the integrated CLI. Default `OFF`. |
| `MINIINFER_SANITIZE` | Enables the same ASan and UBSan build as `KVF_SANITIZE`. On Linux ASan also supports LSan. |
| `MINIINFER_NANO_CUDA_POOL` | `MINIINFER` (default) allocates weights, KV and workspace from one preallocated region. `DIRECT` allocates the same buffers individually with `cudaMalloc`. CUDA support is implemented in this repository. |
| `MINIINFER_WITH_URING` | Enables legacy buffered io_uring and the fixed-buffer path for aligned file records. |
| `MINIINFER_WITH_KVFABRIC` | Default `ON`. Builds the directory service, C SDK and Worker together. |
| `MINIINFER_WITH_SPDK` | `ON` builds the independent SPDK reactor and bdev backend. Set `INFER_SPDK_ROOT` to an installed SDK or a compatible build tree. The SDK's link dependencies must be installed. |
| `MINIINFER_NANO_BLAS` | Uses OpenBLAS when available through pkg-config. Set `OFF` for the scalar CPU kernel. |

```bash
cmake -S . -B build-uring -DCMAKE_BUILD_TYPE=Release \
  -DNANO_WITH_CUDA=ON -DMINIINFER_WITH_CUDA=ON -DMINIINFER_WITH_URING=ON -DMINIINFER_WITH_SPDK=OFF
cmake --build build-uring -j
./build-uring/bin/miniinfer /path/to/stories15M.bin -z /path/to/tokenizer.bin \
  --device cuda --ssd-mode uring --ssd-dir /path/to/scratch-directory --ssd-slots 16
```

On systems with multiple CUDA toolkits, set `CMAKE_CUDA_COMPILER` and
`CMAKE_CUDA_ARCHITECTURES` for the toolkit and GPU. The RTX 5060 Ti measurement
used CUDA 12.8 and architecture `120`.

### SSD prefix cache

Supply `--ssd-slots N` to save complete prefills and completed decode blocks when
they are evicted from the in-memory prefix cache. The engine restores a saved
prefix when a later request matches it. `--ssd-mode posix|uring|spdk` selects
one backend for the run. POSIX and io_uring use a private scratch file. SPDK
opens the configured bdev directly. Both build options can be enabled in one
binary. The selected backend owns the I/O for that run.

This SPDK-only build uses a memory-backed bdev for the example:

```bash
cmake -S . -B build-spdk -DCMAKE_BUILD_TYPE=Release \
  -DNANO_WITH_CUDA=ON -DMINIINFER_WITH_CUDA=ON -DMINIINFER_WITH_URING=OFF \
  -DMINIINFER_WITH_SPDK=ON -DINFER_SPDK_ROOT=/path/to/spdk-sdk
cmake --build build-spdk -j
./build-spdk/bin/miniinfer /path/to/stories15M.bin -z /path/to/tokenizer.bin \
  --device cuda --ssd-mode spdk --ssd-slots 16 --spdk-no-pci \
  --spdk-config third_party/infer-io/tests/storage-io/spdk_malloc.json \
  --spdk-bdev Malloc0 --spdk-offset 0
```

For a dedicated NVMe bdev, use its explicit SPDK configuration and an exclusive
scratch region. The region starts at `--spdk-offset` and must fit the configured
slot count, including sector padding. The CLI prints `ssd_reads` and
`ssd_writes` for completed prefix transfers.

See [Performance and validation](performance.md) for measured results
and commands.

## Basic setup

The [storage example](../examples/basic.c) creates a
CPU pool and a fixed-slot slab with `mi_mempool_create` and
`mi_memory_backend_create`. It writes a 4 KiB block through a WRITE lease,
then commits it and migrates it to a POSIX scratch file.

```bash
./build-debug/bin/miniinfer_basic
```

Finish leases and wait for migration tickets before destroying storage.
Destroy KV caches and page sequences first, then the tiered store, its
backends, and their memory pools.

## Important semantics

- The host and CUDA pools allocate chunks on demand. Set `chunk_bytes` to
  choose the preferred chunk size (default 1 MiB, bounded by the configured
  budgets). The pools use TLSF to find free slices and split at the aligned
  request size. The cache budget and `trim` apply to whole idle chunks. With a
  zero cache budget, chunks are returned when they become idle.
  `mi_mempool_alloc_slab` reserves one dedicated region.
- `mi_storage_create_block` creates an uninitialized CPU/GPU block. A complete
  WRITE lease followed by `mi_lease_finish` commits version 1 (and increments
  subsequent versions). `mi_lease_abort` invalidates the in-place contents.
- Multiple READ leases may coexist. WRITE is exclusive. A lease protects the
  returned pointer only until finish/abort.
- GPU leases require an explicit non-NULL stream. The caller must order all uses
  on that stream. Lease retirement waits through the selected Device HAL.
- `mi_storage_migrate` keeps the logical `BlockId` stable. SSD records are
  validated against block ID, version, size and CRC32C when read back.
- A migration returns `MI_ECLEANUP` after committing the target when the source
  slot still needs cleanup. `MI_EROLLBACK_CLEANUP` leaves the block at the
  source and the target slot pending cleanup. In either case the slot remains
  charged until release. Retry with `mi_storage_retry_cleanup` or `mi_storage_erase`.
- `mi_graph_allocator` only manages the scratch workspace used during capture.
  The application still calls CUDA's stream-capture / graph-instantiation APIs.
  End CUDA capture before `mi_graph_end_capture`; destroy GraphExec objects
  before forgetting graph IDs or releasing the workspace.
- POSIX pinned host allocation uses `mlock`; it can fail under a restrictive
  `RLIMIT_MEMLOCK`. When CUDA is enabled, `MI_MEMORY_CUDA_PINNED`
  provides CUDA-registered host memory instead.
- File backends are process-local scratch caches. The `path == NULL` mode creates
  and immediately unlinks a temporary file. The aligned format rejects stale,
  truncated and corrupt records. Worker restart begins with an empty index and
  refills from a live peer or recomputation.

## Current port boundaries

- The inference CLI supports legacy FP32 Llama checkpoints. Its scheduler can
  manage multiple requests and executes each forward separately.
- SSD spill is synchronous in the inference CLI and targets unused prefix
  blocks. The storage API also offers asynchronous migration and prefetch.
- The io_uring SSD path uses buffered file I/O. The SPDK path uses the native
  bdev API and SPDK-allocated DMA buffers. Neither configuration implies GDS or direct
  NVMe-to-GPU transfers.
- The C pools use aligned slices and a central mutex. The CUDA pool waits for
  each returned slice's stream event.

The imported source is from nano-vLLM-C `060d2b4` (plus the local CLI defaults
edit) and Infer `c7652df`. The nano-vLLM license notices are under
`src/nanovllm`.
