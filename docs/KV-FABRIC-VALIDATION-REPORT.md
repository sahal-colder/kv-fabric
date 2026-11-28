# KV-Fabric 验证与验收报告

> SPDK 更新（2026-11-28）：已完成三星物理 NVMe 的 VFIO 接管、裸盘读写、CPU/GPU 回源、推理缓存恢复、驱动切回与原始区域恢复。CUDA+uring+SPDK 全量 CTest 26/26 PASS。本文历史阶段的“物理 SPDK NVMe BLOCKED”已由[物理盘验收记录](SPDK-NVME-VALIDATION.md)更新；运行中物理拔盘与故障恢复仍未验收。

> 当前交付状态更新（2026-11-26）：已核验既有完整 vLLM/NVMe/RDMA 实测证据，并对修复后的代码完成真实 vLLM 8类用例、3384次GPU层块核验及CPU/CUDA严格SoftRoCE各30轮。先前“当前默认Python无vLLM”和“未配置HCA”的探测只描述当时环境，不能据此否定已完成的独立环境和历史实测。原始阶段记录保留，当前结论以[完整交付指南](PROJECT-DELIVERY.md)与最新复验节为准。

日期：2026-11-26（Australia/Perth）。对象为 `/home/wty/Infra` 的启动本地工作区，HEAD `55e314292bd6a054fa87ab3de6d07fc6bfe2ed66`，分支 `dev`。**19项均已实施修复。#9/#12通过内部所有者安全隔离未知完成的DMA，并明确要求进程退出回收；真实设备丢失/恢复与严格性能SLA仍未验收，不能称为生产级安全认证。最新结果以本文后续修复节为准。**

## 证据与工作区保护

- 启动时 26 个跟踪修改文件；已修改/未跟踪文件共 278 个，约 1.47 GB。补丁、未跟踪清单、逐文件 SHA-256 与完整归档均保留，未 reset/clean/在原工作区切分支，暂存区未变。用户随后授权提交与上传，发布从独立快照生成。
- 保护归档 `/tmp/kvf-audit/preexisting-files.tar`；原始补丁 `/tmp/kvf_pre_audit.patch`，暂存区 `/tmp/kvf_pre_audit_index.patch`。冻结原始业务源码还原到 `/tmp/kvf-audit/original-source`；原始 CPU `build-baseline` 未重建；原始 CUDA 重建在 `/tmp/kvf-audit/original-cuda`。
- 初次与中间失败、全部最终日志、原始 CSV、可复现驱动和源码位置已保存到 [验收证据](benchmarks/audit-2026-11-26/README.md)。原始大归档保留在 /tmp，未把 1.47 GB 用户素材复制进仓库。
- 本轮累计修改85个代码/测试/配置/文档文件：生产源码/接口/构建56个、测试18个、配置与文档11个；不包含原始证据目录，也不把用户原有改动计入本轮。
- 6 个简历相关原有未跟踪文件在任务期间由外部改动，已单独记录并原样保留，不算本轮修改。完整本轮清单见 [audit-changed-files.json](benchmarks/audit-2026-11-26/audit-changed-files.json)。

## 环境

| 组件 | 实测状态 |
|---|---|
| OS / CPU / RAM | Linux 6.8.0-138-generic；i7-13700H，20 逻辑 CPU；约 15 GiB RAM；ext4 NVMe 文件系统 |
| C / CMake | GCC 11.4.0；C11；CMake 3.22.1；无 clang；OpenSSL 3.0.2 |
| CUDA | Toolkit 13.4.92，驱动 615.71.09，RTX 4060 Laptop 8 GiB，架构 sm_89；真实运行可用 |
| io_uring | liburing 2.9；宿主 queue_init 成功；同步取消要求 liburing >=2.3，CMake 已明确限制 |
| RDMA | librdmacm 1.3.39.0、libibverbs 1.14.39.0 可编译；无 HCA/SoftRoCE，真实数据面 BLOCKED |
| SPDK | 实际 SDK SPDK v27.01-pre 4828014 / DPDK 25.11.0；malloc bdev 实测；物理 SPDK NVMe BLOCKED |
| Python | 3.12.15；pytest 9.1.1；torch 2.11.0+cu128；当前环境无 vLLM、nvtx Python 模块 |
| Sanitizer | GCC ASan/UBSan/LSan 可用；TSan 在 `setarch x86_64 -R` 下可用；GPU 使用 NVIDIA Compute Sanitizer |

最初受限执行环境中 socket/bind 返回 EPERM、GPU 不可见、uring queue_init=-1、LSan 不能运行；这些是执行权限限制。在获准使用本机 loopback/驱动后均已实际重跑，不能把沙箱失败算源码基线失败。

## 构建与全量测试

修改前默认 Release 14/14（15.07 s），CUDA/uring 17/17（37.34 s）。Legacy 原有 5 个单元入口在宿主全部通过。最初沙箱 CTest 8/14、6 个网络失败的原始记录仍保留。

最终适用矩阵：

| 构建 | 配置/入口 | 总数 | 通过 | 失败 | 跳过 | 结果 |
|---|---|---:|---:|---:|---:|---|
| CPU Release | build-release，默认 Release | 18 | 18 | 0 | 0 | PASS |
| CUDA/uring Release | build-cuda-release，CUDA/Nano CUDA/uring/bridge，sm_89 | 24 | 24 | 0 | 0 | PASS |
| ASan+UBSan+LSan | build-asan，Debug，KVF_SANITIZE=ON，uring=ON | 19 | 19 | 0 | 0 | PASS |
| TSan | build-tsan，Debug，GCC thread sanitizer，进程 ASLR 关闭 | 19 | 19 | 0 | 0 | PASS |
| SPDK | build-spdk，SPDK=ON，真实 malloc bdev | 20 | 20 | 0 | 0 | PASS |
| 纯 TCP | build-tcp，MINIINFER_WITH_RDMA=OFF | 19 | 19 | 0 | 0 | PASS |
| Storage-only | KVFabric/Nano/Integration=OFF | 10 | 10 | 0 | 0 | PASS |
| Connector 方法 + Legacy 安全 | pytest 精确指定两个文件 | 15 | 15 | 0 | 0 | PASS |
| Legacy 单元 | Make 独立 7 个入口 | 7 | 7 | 0 | 0 | PASS |

- Legacy 的 7 个单元入口在 ASan/UBSan/LSan、TSan 均通过；真实服务安全 5/5 在 Release、ASan、TSan 均通过。
- 既有六类客户端、test_sync_e2e、test_regressions 通过；test_reliability 在 Reactor、Proactor、NtyCo 均通过网络回归；Reactor 的 AOF/复制/快照可靠性在 ASan 和 TSan 下也通过。MMPOOL 的 allocator、多键失败原子性和内部快照测试通过。
- TSan 首轮发现新增 Remote 故障夹具修改 worker immutable 配置的竞态；修复夹具后全量 19/19。没有 suppression、减循环、删断言；首轮失败在 tsan-first-run.*。最新 Direct 隔离状态另外跑了 TSan 三项核心测试×3 轮；最终 io_uring 三重失败隔离/恢复用例在 ASan/TSan 各通过。
- NVIDIA memcheck：Direct CUDA（实际 direct-uring-fixed）与四线程 stream/event 传输，均 **0 errors / 0 bytes leaked**。这是已执行用例的结论，不覆盖任意 CUDA 设备丢失。
- CPU Sanitizer、CUDA memcheck 分开验证；未把 CUDA+TSan、CUDA+ASan 组合或未执行的第三方库组合写成 PASS。
- 生产静态库用 nm 核对，不含 `kvf_directory_test_counters`、`mi_test_*`、`__wrap*` 测试入口。故障注入均为链接 wrap 或测试目标编译宏；Release 没有故障注入分支。

## 并发与故障矩阵

| 场景 | 实测方法 | 结果/边界 |
|---|---|---|
| 同前缀 hide/republish/旧代 invalidate/lookup | 4 线程×10000 轮；新代 Block ID 保留、旧 ticket 失效、最终 leases=0 | PASS；Release 整组重复 20 次，最终 CUDA 配置另重复 10 次；TSan 全量及3轮核心复验 |
| Storage 并发读/写/CPU-SSD 迁移 | 4 线程×10000 轮，校验读无撕裂，最终 lease_count=0 | PASS，同组重复20次，最终配置补测10次 |
| TLSF 随机/碎片/合并/对齐 | 固定种子30万轮，逐字节 canary，整池合并；Release 重复20次 | PASS；查找为两级 bitmap，无空闲链表扫描 |
| 内存分配失败 | allocator 元数据 malloc/calloc 序号注入；Legacy 四索引33个准备失败点 | PASS；原内容、键数、未存在键保持 |
| Lease 过期 | 第一批 FINISH 后等待300 ms，TTL200 ms，再获取第二批 | PASS；旧库复现 STALE，修后完整9页正确 |
| Directory 重启 | 既有 remote_restart 真实进程用例 | PASS |
| RDMA connect/send/recv/reset/partial | 链接 rsocket 替身，实际使用 TCP socket，检查 CRC 与数据 | PASS；不等同 RDMA 硬件验收；batch partial 返回IO、未提交 |
| TCP 超时/取消/损坏 | 既有 remote_faults relay/proxy 与完整性断言 | PASS |
| uring SQE/submit/wait/CQE/EINTR | 测试专用缝隙、真实内核 Ring | PASS；持续中断16次上限；已接受submit失败安全收尾 |
| uring wait+cancel+peek 同时失败 | 保留自有DMA资源，原调用者buffer释放；恢复后原owner再次销毁 | PASS；永久不恢复时资源保留而非强制释放 |
| SSD 复制/清理失败/空间不足 | write/release wrap、旧/新slot/replica检查、retry_cleanup；既有target-full | PASS；原副本不提前丢失，失败目标不提交 |
| Direct NONE/READ_BACK、头/载荷损坏 | depth0/1/64/65，未对齐offset，真ext4文件损坏 | PASS；硬I/O错误隔离整个Direct文件直至重建 |
| CUDA完成错误 | 精确Stream/Event补充确认；无法确认时Storage/Backend/Pool/Segment保留资源，lease/page引用正常收尾 | 安全隔离回归PASS；永久隔离保留量单列，物理设备移除/在线恢复BLOCKED |
| GPU线程并发 | 4线程，各100次H2D/D2H，独立Stream/Event | PASS + memcheck |
| SPDK初始化/关闭/send_msg失败 | malloc bdev，stop线程断言，调用线程send_msg注入EIO | PASS；物理NVMe与真实设备移除BLOCKED |
| Connector指标/Future/partial block | 4000条并发JSONL，真实方法体fixture，C bridge真实8页往返 | PASS；完整vLLM调度集成BLOCKED |
| Legacy远程文件/复制认证 | 任意路径/.. /symlink拒绝；70次V1失败无副本；错证明/防重放/合法主从 | PASS；远程SAVE/LOAD有意禁用；握手不加密载荷 |

## 首轮修复的性能方法和完整原始对照（历史数据保留）

Phase 0 参数始终保留：TLSF 8 MiB/64 B对齐/2000×128 alloc-free/7轮；Page 2层×4096 B，100000次create-append-destroy/7轮；Directory stats 1000 RPC/7轮；SSD和Remote 64 KiB、4客户端、4预热、3×16样本；CUDA 128页×36层=72 MiB、种子734；恢复144×512 KiB、3预热/15样本、逐字节校验。CRC、租约、身份与完整性验证未关闭。

下表“Phase 0/初轮修复单轮”直接对应 `/tmp/kvf_bench_before.txt`、`/tmp/kvf_bench_after.txt`；不删除不利样本。数值为各入口既定输出的中位数；latency越低越好，吞吐越高越好。

| 指标 | Phase 0 | 最终单轮 | 变化 |
|---|---:|---:|---:|
| tlsf_pair | 60.747 | 59.858 | -1.46% |
| page_pair | 65.336 | 69.956 | +7.07% |
| directory_rpc | 22400.870 | 19591.116 | -12.54% |
| buffered_mib_s | 23786.092 | 17741.939 | -25.41% |
| buffered_p50_us | 15.661 | 25.080 | +60.14% |
| buffered_p95_us | 22.258 | 31.824 | +42.98% |
| buffered_p99_us | 33.069 | 38.262 | +15.70% |
| direct_mib_s | 608.483 | 571.654 | -6.05% |
| direct_p50_us | 564.994 | 608.917 | +7.77% |
| direct_p95_us | 1076.443 | 1958.467 | +81.94% |
| direct_p99_us | 1117.490 | 2040.331 | +82.58% |
| remote_mib_s | 1181.588 | 1211.192 | +2.51% |
| remote_p50_us | 414.983 | 420.191 | +1.25% |
| remote_p95_us | 498.666 | 512.014 | +2.68% |
| remote_p99_us | 587.287 | 553.301 | -5.79% |
| cuda_dma_H2D_ms | 7.215 | 6.103 | -15.41% |
| cuda_dma_D2H_ms | 7.734 | 6.082 | -21.35% |
| cuda_segments_H2D_ms | 15.861 | 13.610 | -14.20% |
| cuda_segments_D2H_ms | 15.691 | 11.843 | -24.52% |
| cuda_vector_H2D_ms | 9.197 | 6.848 | -25.54% |
| cuda_vector_D2H_ms | 9.389 | 6.827 | -27.29% |
| recovery_gpu_to_host_ms | 13.266 | 10.999 | -17.09% |
| recovery_host_to_ssd_ms | 28.150 | 24.316 | -13.62% |
| recovery_ssd_to_gpu_ms | 62.320 | 58.270 | -6.50% |
| recovery_effective_recovery_gib_s | 1.128 | 1.207 | +6.95% |

短基准中多项变化超过5%，不能只报有利的一轮。随后以冻结原始CPU二进制和保护快照重建的原始CUDA二进制交替运行8对CPU/Remote、4对CUDA。每次的参数、数据、预热保持一致；不把新的测量冒称“修改源码前采集”。这些是同源码基准的交替复核。

| 交替复核指标 | 原始中位数 | 修改后中位数 | 变化 |
|---|---:|---:|---:|
| tlsf_pair | 59.614 | 61.362 | +2.93% |
| page_pair | 65.354 | 67.149 | +2.75% |
| directory_rpc | 23498.796 | 24399.783 | +3.83% |
| buffered_mib_s | 18847.822 | 18149.018 | -3.71% |
| buffered_p50_us | 22.828 | 23.359 | +2.33% |
| buffered_p95_us | 29.166 | 27.255 | -6.55% |
| buffered_p99_us | 40.414 | 41.624 | +2.99% |
| direct_mib_s | 604.428 | 603.036 | -0.23% |
| direct_p50_us | 613.229 | 610.448 | -0.45% |
| direct_p95_us | 1102.430 | 1096.954 | -0.50% |
| direct_p99_us | 1344.169 | 1136.693 | -15.44% |
| remote_mib_s | 1191.393 | 1153.782 | -3.16% |
| remote_p50_us | 421.181 | 424.734 | +0.84% |
| remote_p95_us | 491.718 | 501.870 | +2.06% |
| remote_p99_us | 541.065 | 536.168 | -0.91% |
| cuda_dma_H2D_ms | 6.131 | 6.094 | -0.60% |
| cuda_dma_D2H_ms | 6.055 | 6.023 | -0.52% |
| cuda_segments_H2D_ms | 13.663 | 13.669 | +0.05% |
| cuda_segments_D2H_ms | 11.865 | 11.811 | -0.46% |
| cuda_vector_H2D_ms | 6.889 | 6.862 | -0.40% |
| cuda_vector_D2H_ms | 6.801 | 6.730 | -1.03% |

交替复核仍确认中间实现的SSD→GPU延迟+5.96%、恢复吞吐-5.63%。根因为安全隔离加入的Ring自有缓冲多了一次完整内存拷贝。改为Direct I/O直接使用Ring拥有的CUDA pinned fixed buffer，普通uring仍使用自有复制缓冲保护调用者生命周期。保留全部CRC、租约与回滚语义。

最终实现（含硬I/O后禁止文件偏移复用）的4对交替恢复结果：

| 72 MiB恢复指标 | 原始 | 最终 | 变化 |
|---|---:|---:|---:|
| gpu_to_host_ms | 11.330 | 11.294 | -0.31% |
| host_to_ssd_ms | 25.797 | 25.180 | -2.39% |
| ssd_to_gpu_ms | 59.049 | 58.140 | -1.54% |
| effective_recovery_gib_s | 1.191 | 1.209 | +1.56% |

判断：已定位的约6%额外拷贝回退已消除。交替组总体中位数中，CPU/Remote/GPU核心指标没有稳定超过5%的回退。但不能签署严格“全部尾延迟均零回退”：Directory逐对变化中位数+5.17%、buffered SSD P99逐对变化中位数+6.60%，与“总体中位数的比值”并不相同。before自身Directory为20.8–27.3 us，buffered吞吐17.9–22.3 GiB/s量级，direct P99为1.09–2.15 ms，明显存在调度/缓存/频率与短样本波动。未触及的buffered I/O/CRC代码也显示同等量级漂移，支持环境噪声推断；未采集足以唯一归因的CPU频率/调度trace，故该推断不当作排除性能风险的证明。严格5% SLA仍需在固定电源/频率/亲和性、足量样本的目标机器复验。

TLSF碎片率仅验证随机负载字节完整性及完全合并后8 MiB整块可分配；没有把“合并后外部碎片0”冒充生产负载碎片率。TLSF请求向上SL量化新增内部开销低于该幂区间的1/32（另加unit对齐）；该分配语义明确记录。

## Nano合成推理补充

用仓库固定seed=7合成模型，同一92-token提示、1预热、12轮、Cold/Local/Remote比较；这是Phase 5用冻结原始源码补充的功能/性能对照，并非Phase 0前已测的生产模型。before/after四组各36样本；所有生成token完全相同。Remote每次命中80/92 token（86.96%），实际Prefill12 token、3次数据请求、20480 B，prefetch失败0；CUDA上下文显存中位98 MiB、设备分配增量4 MiB，修改前后相同。

| 设备/模式 | 原始TTFT P50/P95/P99 (ms) | 最终TTFT P50/P95/P99 (ms) |
|---|---:|---:|
| cpu/cold | 1.701 / 1.727 / 1.727 | 0.942 / 1.706 / 1.706 |
| cpu/local | 0.337 / 0.348 / 0.348 | 0.187 / 0.354 / 0.354 |
| cpu/remote | 1.324 / 1.504 / 1.504 | 1.249 / 1.474 / 1.474 |
| cuda/cold | 5.917 / 6.132 / 6.132 | 5.920 / 6.103 / 6.103 |
| cuda/local | 1.063 / 1.099 / 1.099 | 1.084 / 1.115 / 1.115 |
| cuda/remote | 2.236 / 2.499 / 2.499 | 2.251 / 2.596 / 2.596 |

12样本的P95/P99都是最大值，不代表稳定尾延迟；CPU Cold/Local大幅漂移也显示未控制CPU执行条件的局限。没有把它解读为本轮优化带来的几十个百分点推理提升。

## 复现命令

在当前工作区运行（需要允许本地socket、GPU和uring；先不要覆盖build-baseline）：

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j8
ctest --test-dir build-release --output-on-failure

cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DKVF_SANITIZE=ON -DMINIINFER_WITH_URING=ON
cmake --build build-asan -j8
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-cuda-release -DCMAKE_BUILD_TYPE=Release -DMINIINFER_WITH_CUDA=ON -DNANO_WITH_CUDA=ON -DMINIINFER_WITH_URING=ON -DMINIINFER_BUILD_VLLM_BRIDGE=ON -DMINIINFER_VLLM_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-cuda-release -j6
ctest --test-dir build-cuda-release --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DMINIINFER_WITH_URING=ON '-DCMAKE_C_FLAGS=-fsanitize=thread -fno-omit-frame-pointer -fno-pie' '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread -no-pie'
cmake --build build-tsan -j8
TSAN_OPTIONS=halt_on_error=1 setarch "$(uname -m)" -R ctest --test-dir build-tsan --output-on-failure --timeout 180

make -j8 BUILD_DIR=build-legacy-release
KVS_TEST_BUILD_DIR=build-legacy-release python3 -m pytest -q tests/kvstore/test_audit_security.py tests/vllm/test_audit.py
KVS_TEST_BUILD_DIR=build-legacy-release python3 tests/kvstore/test_sync_e2e.py
KVS_TEST_BUILD_DIR=build-legacy-release python3 tests/kvstore/test_reliability.py
```

其他完整配置、故障注入目标与性能命令在证据README/repro目录。测试源已注册在CMake/Make中；不需要Release运行时调试开关。

## BLOCKED / 未验收与生产差距

1. #9/#12 已补齐Storage→Backend→Pool与Segment的持久所有权：lease栈对象和page ref可收尾，DMA涉及的资源仍由隔离实例持有；快照不向借用目标遗留DMA。无法证明完成时资源保留至进程退出；设备在线恢复未实现，真实设备移除BLOCKED。外部调用者在同一流提交的其他资源仍须自行隔离。
2. 真实RDMA/SoftRoCE、多节点断网/拥塞/并发连接恢复：无HCA，BLOCKED。链接替身与TCP回退不能代替硬件验证。
3. 物理SPDK NVMe、真实设备移除/热插拔、hugepage/PCI部署：仅malloc bdev，BLOCKED；没有向任何原始块设备写入。
4. 完整vLLM V1模型调度/Graph跨进程集成：当前Python缺vLLM与nvtx，BLOCKED。方法fixture、C bridge和Nano E2E不冒充该结果。
5. 生产多卡/长上下文模型与Strict 5%性能SLA：单机合成与短loopback样本不足；尾部波动仍未排除，未签署无回退保证。
6. 永久内核I/O故障会隔离保留Ring内存，安全性优先；需运维重建进程/后端与后续回收治理。故障可恢复测试无泄漏不等于永久故障无资源保留。
7. 原有kv_cache mutex跨部分同步迁移、Legacy多记录WAL无崩溃事务、全局背压/控制面TLS认证/HA目录与故障域治理仍是生产差距。

## 验收自检

| 条件 | 结论 |
|---|---|
| Critical/High安全修复 | 19项均有修复；#9/#12为安全失败并保留资源至进程退出，不声称永久故障自动回收 |
| 本轮已执行测试无越界/重复释放/竞争报告 | PASS（修复夹具后的适用矩阵）；不扩大为所有故障的证明 |
| 永久死锁/自旋 | 未复现；uring错误和EINTR有退出，SPDK close无send_msg自旋；永久设备调用阻塞不作无条件保证 |
| Remote代数/租约/迁移回滚/TLSF | 适用回归与重复压力PASS，边界见逐项账本 |
| RDMA/CUDA所有异常路径 | **未通过硬件全覆盖**：真实RDMA、物理设备移除/在线恢复BLOCKED；连续完成错误的安全隔离已有真实GPU注入回归 |
| Legacy远程文件与未认证复制漏洞 | 已封堵并通过真实服务回归；记录了V2与远程命令兼容性收紧 |
| Release/Sanitizer | 适用矩阵PASS，原失败与修复过程保留 |
| 无无法解释的>5%变化 | **有条件**：确定性拷贝回退已消除；短基准/逐对尾延迟波动仍需受控目标机复核 |
| 文档与数据 | 原始CSV、失败历史、源码范围与哈希清单保留，可复现；未编造BLOCKED结果 |

本轮19项均有安全修复，其中5项在冻结旧库实际复现。永久完成错误使用明确的资源隔离策略，仍要求进程退出回收；硬件矩阵及严格5%性能验收未全部满足。


## 后续修复与最终发布验收

用户追加授权“修复并上传GitHub”后，补齐了#9/#12的跨所有者隔离；之前的PARTIAL/OPEN HIGH描述是首轮历史状态。当前策略为安全失败：Stream/Event均不能确认完成时，Storage/Backend/Pool及相关Segment保持物理资源所有权，writer/lease_count/栈lease与page引用仍正常收尾，实例拒绝继续复用，需进程退出回收。外部使用同一流的其他资源仍由其调用者负责隔离。没有设备reset或全设备同步兜底。

GPU后端提供64个自有pinned缓冲槽，与最大Worker lanes一致；额外Host预留为64×align_up(block_bytes,4096)+最多4095字节，GPU显存预算未改变。公开Host读取在完成后才复制给调用者；迁移已持有两端地址，因此直接搬运到受保护目标，未知完成则保留两端及staging，避免多余拷贝。后端/Graph持有池引用，绑定/活跃Graph期间销毁不会提前释放；池事件等待在锁外，trim/destroy不调用全设备同步。

新增真实GPU用例包含64线程同时快照、lease完成、snapshot、read_to_host、GPU→SSD、SSD→GPU、GPU→CPU的持续完成错误；所有父级析构执行后地址仍存活，失败快照不修改调用者目标。Graph工作区与父池延后销毁有CPU回归；全部断言保留。

| 最终矩阵 | 结果 |
|---|---|
| Release | 18/18 PASS |
| CUDA/uring/bridge | 24/24 PASS |
| ASan/UBSan/LSan | 19/19 PASS |
| TSan | 19/19 PASS |
| SPDK malloc bdev | 20/20 PASS |
| 纯TCP | 19/19 PASS |
| Storage-only | 10/10 PASS |
| Python方法/Legacy安全 | 15/15 PASS |

普通Direct CUDA与四线程Stream/Event memcheck均为0 errors / 0 bytes leaked。持续阻断完成的故障用例，功能断言通过，但memcheck返回99：16个分配、1671158字节被有意保留，ERROR SUMMARY全部为Leaked allocation；没有Invalid/Uninitialized访问报告。**该项是EXPECTED RETENTION，不标为零泄漏PASS**，进程重启回收是明确运行约束。普通成功路径没有该资源保留。原日志和非零结果完整保留。

最终同参数8对CPU/SSD/Remote、4对CUDA结果（没有删去不利样本）：

| 指标 | 冻结原始中位数 | 最终中位数 | 变化 |
|---|---:|---:|---:|
| tlsf_pair | 60.846 | 62.032 | +1.95% |
| page_pair | 70.941 | 67.497 | -4.85% |
| directory_rpc | 21446.780 | 24484.418 | +14.16% |
| buffered_mib_s | 18304.976 | 18360.425 | +0.30% |
| buffered_p50_us | 24.691 | 23.644 | -4.24% |
| buffered_p95_us | 31.661 | 29.182 | -7.83% |
| buffered_p99_us | 43.441 | 37.913 | -12.72% |
| direct_mib_s | 601.590 | 595.000 | -1.10% |
| direct_p50_us | 609.428 | 617.197 | +1.27% |
| direct_p95_us | 1086.409 | 1076.164 | -0.94% |
| direct_p99_us | 1142.284 | 1128.360 | -1.22% |
| remote_mib_s | 1171.063 | 1154.802 | -1.39% |
| remote_p50_us | 426.692 | 427.918 | +0.29% |
| remote_p95_us | 494.261 | 501.148 | +1.39% |
| remote_p99_us | 539.708 | 553.090 | +2.48% |
| cuda_dma_H2D_ms | 6.104 | 6.103 | -0.01% |
| cuda_dma_D2H_ms | 6.058 | 6.052 | -0.10% |
| cuda_segments_H2D_ms | 13.666 | 13.633 | -0.24% |
| cuda_segments_D2H_ms | 11.867 | 11.850 | -0.14% |
| cuda_vector_H2D_ms | 6.860 | 6.851 | -0.13% |
| cuda_vector_D2H_ms | 6.803 | 6.797 | -0.09% |
| recovery_gpu_to_host_ms | 11.149 | 11.137 | -0.10% |
| recovery_host_to_ssd_ms | 24.880 | 24.309 | -2.29% |
| recovery_ssd_to_gpu_ms | 58.496 | 57.803 | -1.19% |
| recovery_effective_recovery_gib_s | 1.202 | 1.216 | +1.20% |

补测固定P核/E核各12对，程序参数与数据相同，亲和性是新增控制条件，不冒充Phase 0原始环境：

| 核类型/指标 | 原始(ns) | 最终(ns) | 变化 |
|---|---:|---:|---:|
| P/tlsf_pair | 64.597 | 66.153 | +2.41% |
| P/page_pair | 72.507 | 74.510 | +2.76% |
| E/tlsf_pair | 108.095 | 109.725 | +1.51% |
| E/page_pair | 111.916 | 113.091 | +1.05% |
| P/Directory（服务端同类核） | 16177.078 | 16325.316 | +0.92% |
| E/Directory（服务端同类核） | 41811.658 | 41735.455 | -0.18% |

GPU→CPU中间+13.96%的确定性回退来自对已拥有两端的迁移再次拷贝；已通过owned迁移路径消除，最终-0.10%。Page中间超过5%的变化促使恢复原reserve锁范围、保持CPU元数据/mutex布局，固定同类核补测在3%以内。最新未固定核的Directory仍+14.16%，固定P/E客户端与服务端分别+0.92%/-0.18%；同机P/E的绝对RPC成本约16/42us，证明异构调度是重大混杂因素。该结果支持调度影响的归因，但没有把亲和性补测当作原始样本的替代，也未采集足以唯一解释每个离群点的调度trace。已解释并消除已定位代码拷贝回退；严格全场景5% SLA仍未签署。

GitHub发布从独立源码快照构建：原工作区不切分支、暂存区不变；发布包含必要的原有RDMA/vLLM项目源码与回归依赖，排除简历、原始模型和构建目录。原生Git无推送凭据，使用已连接且拥有push权限的GitHub账号发布新codex分支，不覆盖dev。历史RDMA证据属于原工作区已有材料，不计作本轮硬件实测。


## GitHub发布范围与自动审批记录

用户已授权修复并上传。自动审批先拒绝原始审计证据，再拒绝含历史个人备份位置的部署文档。发布范围已收窄为代码、测试、通用部署说明与不含主机身份/绝对路径/备份哈希的公开摘要；完整本地两份报告和原始证据保留，公开同名报告为脱敏摘要。

审批拒绝前有部分证据作为未引用Git tree/blob写入远程；没有为这些树创建发布分支。最终公开树从仅源码树重建，不引用原始证据对象；现有接口不能直接删除GitHub未引用Git对象。最终公开树为eae120064f9d20aff04cba5c6ace5d6c452190c6，提交11d744f92aac81b34d5401578ebc1dde32a1fe09；117个变更文件，未新增简历、模型、构建目录或原始审计证据。

- GitHub发布已完成：[草稿PR #1](https://github.com/sahal-colder/kv-fabric/pull/1)，分支`codex/audit-remediation-20261126`，提交`11d744f92aac81b34d5401578ebc1dde32a1fe09`。公开tree干净导出Release18/18、Legacy单元、Python15/15通过；dev与原工作区暂存区保持原状。


## 完整环境与历史证据复核

- 原生Linux历史归档297项中293项与原SHA256一致，4项变化为后续双项目简历/面试材料更新；没有缺失文件或未知证据改动。历史矩阵27格、405请求，345条非参考请求逐Token核验，Nsight/NCU和物理NVMe结果均有原始记录。这些数字对应原测试源码，不冒称审计修改后的新结果。
- vLLM 0.31.0在项目私有venv真实可用，默认办公Python缺少vLLM不是项目能力缺失。修后代码在同一Qwen2.5-3B BF16、VLLM_BATCH_INVARIANT=1条件下，CPU/NVMe冷算、热命中、部分命中、超时回退8类真实服务用例全部通过，3384次GPU层块检查。首次未带batch-invariant失败保留，没有删除Token ID断言。
- 当前内核rdma_rxe模块可用。经授权创建临时独立veth和RXE设备，严格禁止TCP回退，CPU/CUDA各30轮262144字节往返全部匹配；可移植的新测试入口再次完成60轮。没有修改现有上网接口；测试后删除临时网络资源。SoftRoCE不是物理HCA或GPUDirect RDMA。
- Direct uring NVMe复验使用物理NVMe上的ext4私有文件，72MiB/144×512KiB/15样本，保留CRC和逐字节校验；没有写入任何裸块设备。SPDK malloc bdev与Direct NVMe文件路径各自独立，不互相冒充。
- nano-vLLM-C原实现43个源码哈希一致，当前8/8 CTest通过；算子/Decode/Graph和模型128个Token严格一致的原始证据存在于独立项目。没有把尚未完成的两个项目全模型对接写入简历。
- 私人简历PDF、DOCX仅本地；GitHub发布单独脱敏的Markdown/PDF/DOCX。公开测量数据按字段白名单导出，剔除主机身份、绝对路径、原始环境与私有备份。


## 修复后完整矩阵补测

4K/8K/16K × 重算/Native/CPU/NVMe共12格、180请求完成，135条非参考请求逐Token对照通过。每格预热3次、15样本、输出16Token、50%共享前缀；两种KV-Fabric路径实测50%外部命中，加载失败和写入错误均为0。中断的NVMe模式已完整重跑，未混入其未完成样本。

| 模式 | 16K TTFT P50 ms | P95 ms | P99 ms |
|---|---:|---:|---:|
| recompute | 5615.154 | 5618.753 | 5618.753 |
| native | 3207.027 | 3218.541 | 3218.541 |
| kvfabric-cpu | 3273.322 | 3282.575 | 3282.575 |
| kvfabric-disk | 5057.472 | 5355.138 | 5355.138 |

完整数据、实际命中层与save_skipped计数见公开验证目录。n=15的P95/P99为样本最大值。历史及新矩阵不混合统计，也不据不同日期数据签署严格全场景5%SLA。


## 完整交付与复验完成

新增矩阵12格/180请求、135条非参考Token严格一致；CPU/NVMe实际外部命中50%，load/write error=0。严格SoftRoCE与完整vLLM、物理NVMe均已完成当前代码复验。公开提交ddb5883fab85a1752635c2394d05c060f9a0cbf7，tree349b4284f90871af3b0907f19618f9a00ae8a177。

本地dist下提供双项目源码与私人简历ZIP，以及单独的原始证据tar.zst；源码包598个文件SHA256核验、ZIP自检和证据zstd完整性检查通过。原模型、构建目录和私有虚拟环境不重复打包；不包含凭据。GitHub只发布经审阅脱敏的简历和测量数据。原工作区HEAD/暂存区保持原状。

- 完整交付已合并到GitHub默认dev：90ea96d611151a4992abc8fbe3c2b129e4dace3d；PR#1已合并，head ddb5883fab85a1752635c2394d05c060f9a0cbf7的Release/Debug Sanitizer远程CI均成功。公开tree349b4284f90871af3b0907f19618f9a00ae8a177与本地独立验证一致。
