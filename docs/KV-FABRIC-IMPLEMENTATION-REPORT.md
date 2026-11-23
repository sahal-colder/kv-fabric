# KV-Fabric 实现与实测报告

实现状态更新于 2026 年 11 月 23 日。基准采集于 2026 年 11 月 22 日。
根目录统一构建和安装 Wire V3 控制面、CPU/CUDA Worker 与运行工具。交付包含对齐文件层和双机演示入口。当前环境完成了 CPU/CUDA
常规回归和 WSL ext4 实测。原生物理 SSD、RDMA 硬件与 CUDA 严格
Sanitizer 的验收边界见下文。

## 起始状态与清理

| 合并前项目 | 起始状态 | 基线 |
| --- | --- | --- |
| `kvstore/` | 当前目录缺少 Git 元数据，分支和 `git status` 无法核实。沿用目录内的现有源码。 | KV-Fabric 2/2。通用单元、Allocator、四种索引、RESP2、持久化、Reactor、Proactor、NtyCo、TCP 复制通过。 |
| `miniinfer-c/` | `dev` 与本地 `origin/dev` 同指 `cb17fd4`。起始已有产品目录重组和未提交的文件删除、修改、新增。 | 默认 CPU 6/6。含 KV-Fabric 的 CUDA + io_uring 11/11。 |

首次通用 KVStore 验证读取了工作区旧二进制。将测试目录复制到 Linux ext4 并
指向本轮新构建的二进制后，旧二进制的 AOF 复制配置错误未复现。
清理了重复的旧测试入口及可再生 Python 字节码。坏命令、参数错误和索引操作
继续由分项回归覆盖。KV-Fabric 构建的磁盘和网络共用一份 CRC32C 实现。
独立 MiniInfer Storage 继续使用原有的 Infer CRC32C 依赖。已保留既有
项目重组、案例、测试证据和其他用户产物。

KVStore 默认使用 libc。系统的可选 jemalloc 依赖 `libstdc++`。
`NANO_WITH_CUDA` 单独控制已有的 nano-vLLM CUDA 推理内核，默认关闭。
KV-Fabric 的控制面、Worker、设备层和新增测试均用 `-std=c11`。
数据面使用 CUDA Runtime API 搬运张量。

## 架构与不变量

```text
kvfabricd
  Wire V3 / TCP RPC → Worker TTL → Page Replica Directory → Epoch + Lease Fence
             ↑                         ↑
             └── 心跳、发布、选择、校验、退休 ──┘
 Producer Worker ─── TCP Layer 帧 ─── Consumer Worker
       ↕                                   ↕
  Storage READ → Segment           Segment → WRITE → Trie → 副本发布
       ↕
  CPU / CUDA，4 KiB 对齐文件记录
```

- Namespace 完整编码模型、Tokenizer、布局、Dtype、页形状、并行分片和租户信息。
  固定的显式大端序编码参与 SHA-256 链式身份计算，Layer 与帧分别按 CRC32C 核对。
- 完整 Layer 表是发布单位。目标写入全部层并结束 WRITE Lease 后才封页。
  本地 Trie 和控制面各有一个原子提交点。心跳补发未确认的导出。
- Lease 绑定 Epoch、Worker Boot、Fence、Manifest 修订及具体层的
  ID、Generation、Version、长度与 CRC。源端在 Storage 锁内取得 READ，
  放锁后复制，释放 READ 后才发送。目标在最后一层完成后重新校验租约。
- 连接按 Endpoint 与 Boot 复用。观察到新的 Boot 时，旧空闲连接关闭，
  活动连接退出后关闭。CUDA 的两个目标 Segment 可让
  一层的 H2D 与下一层接收交叠。CPU 直接接收进暂存 WRITE 槽。
- Trie 与 Directory 在容量压力下按 LRU 挑叶子，跳过有引用或租约的候选。
  Directory 先按连续命中长度选择副本，再按心跳负载打破平局。
  Remote Prefetch 使用单线程、有界队列和页/层边界的取消。
- 控制面有界、默认回环监听。Tensor 只经 Worker 数据端口传输。
  文件层用 4 KiB 头与对齐负载保存 Block 身份和 CRC。可用时使用
  `O_DIRECT + io_uring READ_FIXED/WRITE_FIXED`。错误记录可丢弃重算。
  Worker 崩溃后从空索引启动，存活 Worker 在新目录 Epoch 下重新注册和发布。

### 主要源码

| 路径 | 职责 |
| --- | --- |
| `src/control/{identity,wire,net,crc32c}.c` | 跨架构身份、字段编解码、Deadline 与 CRC32C |
| `src/control/{control,directory,client}.c` | 元数据 RPC、Worker TTL、Leaf LRU、副本路由与票据 |
| `include/miniinfer/device_transfer.h` | CPU/CUDA 共用 C 接口 |
| `src/worker/device_transfer.c` | C Runtime 分发、流、事件、主机与设备内存 |
| `src/worker/{segment_pool,direct_io,uring_io}.c` | 固定 Segment、对齐文件格式与注册缓冲区 |
| `src/worker/{storage_backend,tiered_storage,paged_kv}.c` | 物理槽、Block 租约、迁移、Trie 与本地 Leaf LRU |
| `src/worker/remote/{worker,cache,peer,transfer,prefetch,cli}.c` | 心跳重放、发布、回滚、长连接、DMA 与后台 Prefetch |
| `tools/{fixture,kvf_bench}.c` | 两页 × 两层 FP32 夹具、Attention 校验及基准 |
| `scripts/run_hetero_demo.sh` | Node A / Node B 的安装版入口与指标输出 |

[从 0 阅读代码指南](READING-GUIDE.md) · [运行命令](kvfabric-data-plane.md)

## 本轮验证

测试主机为 WSL2 Ubuntu 22.04，Linux `6.18.54.1-microsoft-standard-WSL2`、
GCC 11.4.0、CMake 3.22.1、Intel Core Ultra 7 265KF、RTX 5060 Ti 16 GiB、
CUDA 12.8。以独立 Linux ext4 构建目录完成验证。

| 门禁 | 结果 |
| --- | --- |
| 控制面 Release | 2/2。含帧损坏、长度边界、短读写、超时、SHA-256/CRC32C、并发目录查询、Leaf LRU、租约与副本选择。 |
| MiniInfer 默认 Release | 7/7。包含原有六项、无 io_uring 编译的 CPU 对齐文件路径。 |
| MiniInfer CUDA + io_uring + KV-Fabric Release | 13/13。CPU 和真机 CUDA 各 5 档，4 KiB 至 4 MiB。每档 2 页 × 2 层，回读 Tensor、SHA-256 和 Attention 完全一致。CUDA 与 CPU 文件测试均观察到 fixed-buffer 模式。 |
| 故障与压力 | 源 Worker `SIGKILL`、Worker Boot 更新、目录重启与新 Epoch、短帧、坏 CRC、租约过期、传输期间撤销、Generation 复用、叶子淘汰、预取队列上限与取消、连接池 Boot 隔离和 idle 回收均通过。文件测试验证 CRC 损坏及截断后的失败和槽复用。 |
| 通用 KVStore | C11 Release 与 ASan/UBSan/LSan 构建均通过单元、四种索引、RESP2、AOF/Snapshot、TCP 主从复制和三个网络后端的回归。两种构建的 eBPF Sockhash 端到端也通过，使用隔离的临时 bpffs。MMPOOL 和 jemalloc 的分配器及单元测试通过。NtyCo 可靠性流程沿用原有信号终止。 |
| CPU Sanitizer | 控制面 2/2，MiniInfer Storage + KV-Fabric 11/11。启用 ASan、UBSan 和 LSan。 |
| CUDA 严格 Sanitizer | 10/13。失败项为 `gpu_mock`、`direct_io_cuda`、`remote_e2e_cuda`。独立 C11 CUDA Runtime 探针复现 `cudaGetDeviceCount` 返回 2 和 LSan 50,960 字节、5 次分配，部分调用栈位于 WSL `libcuda.so.1`。关闭 LSan 的诊断仍复现初始化失败。常规 CUDA Release 为 13/13。 |
| ARM64 跨架构 | GCC 交叉编译加 QEMU 6.2 运行控制面两套测试成功。x86_64 的真实 CUDA Producer 向 ARM64 模拟的 CPU Consumer 传输 4 KiB 与 4 MiB，SHA-256 和两层 FP32 Attention 逐字节一致。4 KiB 用例以已写入对齐文件的 CUDA 数据作为源。 |
| 动态依赖 | 对安装版 `kvfabricd`、`kvfctl`、`miniinfer_worker`、`kvf_bench`、`miniinfer`、`nanovllm` 与通用 C11 `kvstore` 执行 `ldd`，均无 `libstdc++`、`libc++` 或缺失依赖。 |

11 月 23 日重新构建控制面和启用 KV-Fabric 的 CPU、CUDA 版本，CTest 分别通过
2/2、11/11、13/13。安装版按演示脚本的默认配置跑通 CUDA 到 CPU 的本机回环，
4 KiB 层的 Tensor、SHA-256 和两层 FP32 Attention 逐字节一致。

合并根目录后复测通过 CPU 13/13、CUDA + io_uring 15/15、CPU ASan/UBSan/LSan 13/13。
独立存储构建通过 7/7。通过两个安装入口导出的 SDK 分别由外部 C11 工程链接验证，
安装版 CUDA 到 CPU 回环通过 Tensor、SHA-256 和 FP32 Attention 一致性检查。
通用 KVStore 回归覆盖四种索引、RESP2、持久化、三个网络后端、TCP 复制及隔离 bpffs 下的 eBPF Sockhash。

## Benchmark

文件测试在 WSL 的 `/dev/sdd` ext4 上进行。固定路径的全部 20 组均实际注册了
缓冲区并执行 O_DIRECT。Buffered POSIX 的结果包含页缓存的作用。TCP 指标
来自同一台主机的回环，覆盖 CPU 和真实 CUDA、短连接逐层和长连接流水线。
每种模式跑 5 档大小 × 4 档并发。每组合三轮，共 360 轮，含 76,320 条
请求级样本。已从原始 CSV 复算 P50/P95/P99、有效载荷吞吐和样本数。
逐轮汇总还保留源进程、目录和客户端的累计 CPU 时间及 RSS 峰值。

下表取 4 MiB、并发 32 的三轮中位数。文件计时为一写一读的合计有效载荷。
TCP 计时包含两层传输、租约和目标端字节校验。

| 场景 | 前一模式 → 后一模式 | 吞吐 MiB/s | P99 ms |
| --- | --- | ---: | ---: |
| 文件读写 | Buffered POSIX → Direct io_uring fixed | 11,114.29 → 5,202.64 | 49.405 → 101.941 |
| CPU TCP | 短连接 → 池化长连接 | 1,737.48 → 1,878.06 | 207.665 → 188.103 |
| CUDA TCP | 短连接逐层 → 池化长连接与双缓冲 | 1,639.46 → 1,725.65 | 222.370 → 202.953 |

[全部对比表和原始 CSV](benchmarks/kvf-ultimate-comparison.md)

合并后在 WSL ext4 重跑 SSD 的 5 档块大小 × 4 档并发。Buffered POSIX 通过全部组，
其中 Direct 的 18 组使用 io_uring Fixed Buffers。锁页额度为 64 MiB，4 MiB 的并发 16
和 32 两组无法注册全部缓冲区，实际回退到 O_DIRECT POSIX。上表引用的是前次测量。

## 复测入口

从仓库根目录执行。以下命令将当前提交解包到 Linux ext4，再构建和测试。

```sh
scratch=$(mktemp -d /tmp/kvfabric-verify.XXXXXX)
export TMPDIR="$scratch"
findmnt -n -o SOURCE,FSTYPE --target "$scratch"
mkdir "$scratch/source"
git archive HEAD | tar -xf - -C "$scratch/source"
cd "$scratch/source"

make -j8 ALLOCATOR=libc KVFABRIC=1 NTYCO=1 RDMA=1 BPF=1 CLANG=clang-14 \
  all test_unit test_allocator bpf install_kvfabric
python3 tests/kvstore/test_regressions.py
for network in reactor proactor ntyco; do
  KVS_TEST_NETWORK="$network" python3 tests/kvstore/test_reliability.py
done
python3 tests/kvstore/test_sync_e2e.py
# 有相应内核权限时，在独立 mount namespace 中运行 tests/kvstore/test_sync_e2e.py --ebpf。

cmake -S . -B "$scratch/cuda" -DCMAKE_BUILD_TYPE=Release \
  -DMINIINFER_WITH_CUDA=ON -DMINIINFER_WITH_URING=ON
cmake --build "$scratch/cuda" -j8
ctest --test-dir "$scratch/cuda" --output-on-failure -V
cmake --install "$scratch/cuda" --prefix "$scratch/install"
ldd "$scratch/install/bin/miniinfer_worker"
```

通用 KVStore 的 Sanitizer 构建使用 `make SANITIZE=1`。控制面和 Worker
共同使用根目录的 `-DKVF_SANITIZE=ON`。运行时设置
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1` 和
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`。基准和双机入口见
[数据面部署与复测](kvfabric-data-plane.md)。

## 验收边界

- 双机脚本的 CUDA Producer 和 CPU Consumer 通过本机回环验证。
  双物理机的吞吐和 P99 仍需在实际网络中测量。
- 当前 ext4 依附 WSL 虚拟盘。原生 Linux 上的物理 NVMe 或 SSD 指标需要重新
  测量。直接文件是本地缓存。冷 Worker 通过重拉或重算恢复，持久化的 Trie
  检查点和完整跨 Worker 恢复仍未实现。
- 通用 KVStore 的 rsockets 路径编译并链接成功。当前系统没有 InfiniBand
  设备，真实 RDMA 复制未执行。KV-Fabric 数据面实测使用 TCP。
- 独立的远程 Worker 和 Connector 已验证。nano-vLLM 调度器当前仍使用既有
  的单机缓存与溢写入口。
