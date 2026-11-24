# KV-Fabric 全仓代码审计

## 源码与性能基线

审计对象为 `sahal-colder/kv-fabric` 的 `dev`。纯 HEAD 与 `origin/dev` 均为 `02fe2db383bad59dc614d8dd124d21f976ae0869`。另存一份开始时的工作区快照，用于保护既有优化并测量重构前后的变化。快照记录了 250 个跟踪路径，其中 31 个有未提交修改，另有 19 个未跟踪文件。可逐个核对原始 Git 状态与修改文件的 SHA-256。[Git 状态](benchmarks/refactor/baseline/git-status-before.txt) · [原有改动清单](benchmarks/refactor/baseline/workspace-before.csv)

NtyCo 的原有改动涉及 `third_party/NtyCo/core/nty_coroutine.c`、`third_party/NtyCo/core/nty_coroutine.h` 和 `third_party/NtyCo/core/nty_socket.c`。其余原有改动包括目录批量授权、网络帧、Worker 连接池、Nano 异步发布及相关测试，还有两份优化文档、原始数据和基准脚本。重构在快照副本上开展，写回时以快照核验原补丁及未跟踪文件。

纯 HEAD 由 `git archive` 展开，使用自身的基准入口。优化快照使用原工作区的 `scripts/benchmark_nano_remote.py`。两组在 WSL2 ext4 上，以同一 `stories15M.bin`、词表、IPv4 TCP 回环和采样参数分别测量 CPU 与 RTX 5060 Ti。每个设备各跑 30 轮，每轮轮转 Cold、Local、Remote。纯 HEAD 记录第一次生成后调度返回的时刻。优化快照每个消费进程另预热一个无关请求，记录首次采样完成时刻。后续零回退判断以采用相同计时点和预热条件的优化快照为对照。

| 源码 | 设备 | Cold P50 / P95 / P99（ms） | Local P50 / P95 / P99（ms） | Remote P50 / P95 / P99（ms） |
| --- | --- | ---: | ---: | ---: |
| 纯 HEAD | CPU | 133.940 / 143.608 / 143.965 | 26.183 / 26.918 / 27.233 | 101.886 / 112.319 / 115.058 |
| 纯 HEAD | CUDA | 15.847 / 17.399 / 19.334 | 6.939 / 7.558 / 7.811 | 82.477 / 88.451 / 90.314 |
| 优化工作区快照 | CPU | 133.704 / 138.852 / 141.688 | 26.356 / 27.664 / 28.849 | 32.812 / 35.233 / 35.278 |
| 优化工作区快照 | CUDA | 15.880 / 17.358 / 18.190 | 6.841 / 7.398 / 7.724 | 12.733 / 14.030 / 14.073 |

优化快照两个设备的每个 Remote 样本均跳过 20 个提示 Token、导入 5 页、发起一次数据 RPC、接收 276,480 Bytes。CUDA 调度恢复 P99 为 0.166 ms。两份快照的全部样本均生成相同的八个 Token ID。优化快照 CPU/CUDA Release 测试分别通过 14/14 和 17/17。每组 30 个样本采用最近秩分位数，P99 为该组最大值。[HEAD 环境和样本](benchmarks/refactor/baseline/head/) · [优化快照环境和样本](benchmarks/refactor/baseline/working-tree/)

## 主要调用链

| 模块 | Public API → Core → Internal → 资源 | 守住的边界 |
| --- | --- | --- |
| Control SDK 与帧协议 | `kvf_client_*` → `kvf_rpc` / `kvf_rpc_on_timed` → `kvf_send_frame` / `recv_frame` → `sendmsg` / `recv` / `poll`。入站由 `kvf_server_start` → `control_request` → `dispatch` 处理。 | 固定 V3 帧布局，保留请求关联、操作码、长度上限、帧头和数据 CRC32C、单调时间截止点。 |
| Namespace、Directory 与 Lease | `kvf_identity_hash` / `kvf_page_hash` → `put_namespace` → 大端 Wire codec → OpenSSL SHA-256。`kvf_directory_lookup_prefix` / `kvf_directory_acquire_batch` → `select_replica` / `grant_page` → Directory 桶链、Worker TTL 与租约表。`kvf_directory_check_batch` → `find_lease` / `lease_live`。 | 保留最长前缀、负载择优、父链检查、批次第一个缺口及可见性。Directory Epoch、Worker Boot、Fence/Secret、层 Generation/Version 共同约束复用。 |
| Remote 取页与预取 | `mi_remote_prefetch_async_after` → `enqueue` / 预取线程 → `mi_remote_resolve_after` → `resolve_batch` → `kvf_client_acquire_batch` → `mi_remote_receive_batch` → `mi_peer_borrow` / `kvf_client_fetch_batch_on` → TCP。`stage_batch_page` → 已验证层的 WRITE lease。旧的 Worker API 仍可进入 `resolve_suffix` → `mi_remote_import`。 | 区分批量 CPU 暂存路径与逐层兼容路径。保留总截止点、取消、完整页回滚、Batch CRC 和每层 CRC、Boot 绑定的 socket，以及空闲连接失败时的一次有限重连。 |
| Remote 导出、发布和撤销 | `mi_remote_publish_page` → `export_page` → `mi_page_ref` 与层元数据 → `mi_remote_sync_locked` → Directory。`mi_remote_serve` → `send_batch` / `send_layer` → 克隆页引用 → `mi_storage_read_snapshot_event` → 源端 staging、发送前租约与导出状态复查。 | 保留异步重试、隐藏与撤销顺序。RPC 回复丢失时，`PUBLISHING` / `RETRY` 的导出仍须通过实时 Lease 才可服务。网络收发在 Trie 和 Storage 元数据锁外。 |
| Storage、Paged KV 与文件层 | `mi_kv_sequence_publish` / `mi_paged_kv_acquire` → Trie 和页引用 → `mi_storage_acquire` → `mi_backend_memory_view`。`mi_storage_migrate` → `move_payload` → `ssd_transfer` → `mi_posix_transfer` / `mi_uring_transfer` / `mi_spdk_transfer`；`mi_direct_*` 使用对齐 Segment 和可选固定缓冲区。 | 仅提交完整页。固定原槽和 Generation/Version，写入成功后才切换位置。保留回滚、延迟清理和 SSD 完整性验证。 |
| TLSF 和设备层 | `mi_mempool_alloc` → `allocate` → `bin_find` / `split_block` → `raw_alloc` → `mi_device_*`。`mi_mempool_release` → `release_block` / `release_binned` → `merge_next`，异步释放等 Event 完成才复用。`mi_segment_pool_create` 为每槽分配对齐视图和所需 DMA 资源。 | 保留两级位图、物理相邻双向链表、常数次邻块合并和 Host 元数据。CPU 地址与 Device 地址的访问路径由设备层分开。 |
| Nano 推理与本地前缀 | `nv_cli_run` → `nv_engine_add` / `nv_engine_step` → 等待和运行位图、`nv_cache_reuse` / `nv_cache_allocate` → `nv_forward` → CPU 矩阵向量运算或 `nv_cuda_forward` 的 CUDA Graph。`nv_spill_evict` / `nv_spill_restore` → Local SSD record 与同一页布局。 | 保留 Token 预算、序列抢占、最后提示 Token 的 logits、LRU、K/V 布局和独立请求 RNG。 |
| Nano Remote Adapter | `nv_engine_enable_remote` / `nv_remote_begin` → Namespace、父前缀哈希和准入模型 → 私有预取 → `nv_remote_poll` → `reserve_pages` / `unpack_pages` → CPU 页或 pinned staging → `nv_cuda_remote_upload` / `nv_cuda_remote_wait` → `finish_import` → 本地索引。`nv_remote_publish` → 快照 → `publish_pages`。 | 导入沿独立 Stream 提交相邻页复制，使用一个完成 Event。完成和 Lease 再验之前，序列不可消费导入页。调度不等待逐层 DMA。 |

## 文件归属及引用核查

扫描范围为开始时的 269 个文件。CPU 与 CUDA 的 `compile_commands.json` 分别包含 54 和 55 个不同的翻译单元，均启用了 `io_uring`。对清单外的源码继续核对各子目录的 CMake、Makefile、`#include`、安装入口、测试注册、脚本调用及文档引用。未找到逐字节重复的文件或无 `#include` 引用的头文件。

| 范围 | 构建与独立入口 |
| --- | --- |
| `src/control`、`include/kvfabric`、`src/worker`、`include/miniinfer` | 根 CMake 分别组装 `kvfabric`、`miniinfer_storage` 和 `miniinfer_remote`。构建可关闭 KV-Fabric，单机存储此时仍需要 `third_party/infer-io/src/base/crc32c.c`。`uring_io.c` 和 `spdk_bridge.c` 各自受开关控制，SPDK JSON 由部署文档实际引用。未发现 AscendCL/CANN 构建分支。公开头文件和 CMake 安装包是 SDK 入口。 |
| `src/nanovllm`、`src/integration`、`include/nanovllm` | 独立 `nanovllm` 和集成 `miniinfer` 共享源码，后者额外编译 `nano_spill.c`。CPU 内存池、CUDA 内存池、OpenBLAS 和 OpenMP 各有条件分支。两套 CLI 的编译职责依然不同。`src/nanovllm/LICENSE` 与第三方通知的版权主体不同。 |
| `src/{network,protocol,config,replication,store,memory,redis,bpf}`、`tests/kvstore`、`clients`、`config` | Make 管理旧 KVStore 及其测试、`NTYCO`、`PROACTOR`、RDMA、BPF、MySQL、分配器和 KV-Fabric 控制面组合。客户端示例、配置样例、NtyCo 上游构建和示例独立于根 CMake。没有把 CMake 未覆盖的文件判为废弃。 |
| `tests/{control,worker,nanovllm}`、`examples`、`tools` | `kvf_protocol` / `kvf_directory` 覆盖 Wire 和目录，Worker 用例覆盖分配、存储、迁移、故障与重启，Nano Python 入口对真实进程校验生成结果和 KV 页。`examples/basic.c` 构建为 `miniinfer_basic`。`kvfabricd` 启动控制面，`kvfctl` 通过 SDK 查目录，`kvf_bench` 与 `test_remote_e2e` 共享 `tools/fixture.*`。 |
| `scripts`、`docs`、`docs/benchmarks` | `run_hetero_demo.sh` 运行双 Worker 演示，`run_kvf_benchmark.sh` 采集传输和 SSD 指标，`benchmark_nano_remote.py` 采集推理 TTFT。阅读指南、独立存储教程、协议、部署和验收材料有各自用途。历史原始样本作为复测证据。`tools/download.py` 是教程和 tokenizer 错误提示中的残留引用，仓库清单中没有该文件。 |

## 拟删除多余文件清单

| 文件 | 冗余证据 | 引用核查与处置 |
| --- | --- | --- |
| `docs/miniinfer-storage.md` | 单机存储简介、构建、示例、CTest 和三个可选设备/文件开关可以在主文档完整承接。 | 文件名仅在 `docs/miniinfer.md` 出现，CMake、Make、源码、CLI 和脚本均无引用。把独立构建命令移入主文档并更新链接后删除。 |
| `src/control/crc32c.c` | 与 `third_party/infer-io/src/base/crc32c.c` 的查表初始化、软件查表和 SSE4.2 三路实现相同。两个目标的导出符号和 CPU 特性探测方式有差异。 | 控制面 CMake 当前编译该文件，阅读指南引用路径。合并到另一实现，按现有产品配置分别编译原符号及探测路径，移除 CMake 和文档旧引用后删除。用协议及独立 Storage 构建验证。 |
| `src/worker/cuda_adapter.c` | 五个旧入口经 `mi_device_*` 完成全部工作，该文件无资源所有权。 | Worker CMake 当前编译该文件，存储教程链接该路径。原函数和 CPU 构建下的返回值并入设备层，保留公开 `cuda_adapter.h`。更新两处旧引用后删除，再检查 CPU/CUDA 库导出。 |

## 代码级处置

| 文件与函数 | 问题类型 | 证据与影响 | 处置决策 |
| --- | --- | --- | --- |
| `src/control/crc32c.c`、`third_party/infer-io/src/base/crc32c.c` 的 `sw_run` / `hw_run` | 重复实现 | 初始化、SSE4.2 三路折叠和软件回退重复维护，影响帧与存储 CRC。 | 共用源码，保持两种产品的导出符号、逐字节 CRC 语义和热路径。 |
| `src/control/wire.c` 的 `kvf_get_bytes` / `kvf_put_bytes` | 重复实现 | 都检验当前位置和剩余范围。顺序反转会使 `size_t` 减法下溢。 | 共用检查，条件顺序与零长度处理保持一致。 |
| `src/control/directory.c` 的 `kvf_directory_lookup_prefix` / `kvf_directory_acquire_prefix` | 重复实现 | 双方各校验一次候选指针、数量范围和 namespace，规则存在漂移风险。 | 合并公共输入检查。保留查询与授予的锁、TTL、更新触碰次数和批量路径。 |
| `src/control/net.c`、`src/control/client.c` 的 Frame / RPC | 无意义透传包装候选 | `kvf_recv_frame`、`kvf_rpc_on` 等有多处使用且已导出。`kvf_client_*` 的薄入口分别承担不同协议操作及错误语义。 | 保留导出和语义入口。Frame 长度校验、收发两次 CRC 和请求 ID 核对分别保护不同边界。 |
| `src/worker/remote/worker.c` 的 `release_worker` / `mi_remote_worker_stop` 与 `internal.h` | 冗余结构体字段及重复实现 | 成功返回的 Worker 必有已启动的心跳线程；失败路径已有启动阶段清理。释放函数再次停已停的队列和服务。 | 删除私有启动标记，明确只在队列和服务退出后释放存储；保持先停预取、再排空服务的生命周期。 |
| `src/worker/remote/peer.c` 的 `mi_peer_borrow` | 冗余判断 | Boot 比较对不同地址与空槽没有意义。选中健康复用连接时，地址、Boot 与 Grant 已相同。 | 仅在地址相同时验证 Boot，保留原槽位优先级、健康检测、Boot 切换和锁外连接。 |
| `src/worker/remote/prefetch.c` 的 `mi_prefetch_start`、`enqueue` / `mi_remote_prefetch_destroy` | 重复实现 | 启动的失败分支和 Ticket 析构都存在重复清理。启动的 Event FD 注册与失败收尾来自原有未提交补丁。 | 统一队列拒绝和公开销毁的 Ticket 释放。保留启动路径和现有等待、取消及完成通知顺序。 |
| `src/worker/remote/transfer.c` 的 `copy_export` / `copy_batch_page`、`cache.c` 的页字节计算和状态提交 | 重复实现 | 单层与批量共享快照/CRC 原则，批量中多处按层数计算字节。批量实现占原有未提交补丁的较大部分。 | 优先整理原补丁外的兼容路径与公共生命周期。批量数据检查、布局、逐层校验、完成前 Lease 复查和测点原样保留。 |
| `src/worker/segment_pool.c` 的 `mi_segment_pool_create` / `segment_pool.h` | 冗余结构体字段 | `base` 只在初始化槽地址时读取。`stride` 则用于 Direct I/O 的偏移和尾部补零。 | 将 `base` 降为构造局部变量，保留 `stride`、对齐、锁无关引用计数和池外 ABI。 |
| `src/worker/device_transfer.c`、`src/worker/cuda_adapter.c`、`src/nanovllm/nano_cuda.cu` | 无意义透传包装及重复实现 | 旧适配器只有兼容符号；设备层在 `.c` 中包含 CUDA Runtime。Nano 的 RoPE 构造使用 STL 容器。 | 合并兼容符号，Runtime 调用限于生产 `.cu`。用现有显式分配替代 RoPE 容器，释放点位于现有 Stream 完成之后。保留 pinned staging、Event 和错误收尾。 |
| `src/nanovllm/engine.c` 的 `nv_engine_cancel` / `allocate_block` | 重复实现 | 取消和抢占分别维护活动位图、取消预取、归还页及减少活动计数。 | 共用活动请求退出步骤，保留两种路径的终态、重入队伍和抢占顺序。 |
| `src/nanovllm/cache.c` 的 `nv_cache_key` / `nv_cache_value`、`nano_spill.c`、`cli.c`、`nano_remote_kv.c` 的页尺寸 | 重复实现 | K/V 地址共享相同 stride 算式，多处从元素总数和块数求完整页体积。 | 共用地址计算和每页元素数。保持 SSD 双暂存区和 Wire 所用每层 K/V 布局。 |
| `src/nanovllm/nano_remote_kv.c` 的 `prefix_hash` / `nv_remote_publish` | 重复实现 | 发布重试或者已被序列用于父链的物理页可以已有缓存的 SHA-256，发布仍重算当前页。 | 复用物理页对应的完整 digest，槽淘汰时已清空；缺失时按原规范计算。短哈希作为本地索引，SHA-256 用于跨进程身份，二者都保留。 |

## 验证边界

基线在真实 GPU 和 CPU 上测量回环 TCP，构建启用了 `io_uring`。物理网络、RDMA、物理 NVMe 及多卡环境需要各自的实测条件。最终性能比较以同轮 CPU/CUDA Cold、Local、Remote 的 P50/P95/P99 审核，阈值为 P50 ≤ 3%、P95/P99 ≤ 5%。

清单中的三个文件已在合并内容并更新引用后删除。最终工作区增量、Sanitizer 结果、同轮性能对照及环境限制已归档。[重构与验证报告](KV-FABRIC-REFACTOR-REPORT.md)
