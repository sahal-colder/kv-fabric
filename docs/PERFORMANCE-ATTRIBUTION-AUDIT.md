# KV-Fabric 技术口径与性能归因审计

审计日期：2026-11-26，Australia/Perth。对象是当时的真实工作树，不是旧的 `git HEAD`。本公开版本包含修复、复现脚本、逐请求测量与计数白名单；完整 nsys/ncu、SQLite 和主机日志保留在本地，不上传。公开数据与来源校验见 [`benchmarks/perf-attribution-2026-11-26/README.md`](benchmarks/perf-attribution-2026-11-26/README.md)。

可审阅的实现 delta：[optimization.patch](benchmarks/perf-attribution-2026-11-26/optimization.patch)；独立核对结果：[validation-summary.json](benchmarks/perf-attribution-2026-11-26/validation-summary.json)；[四条简历改写](resume_kvfabric_audited.md)；[README 图片与导出清单](README-VISUALS.md)。公开简历 Markdown 的 KV-Fabric 段落已同步更新；已有 PDF/DOCX 为本轮审计前版本，未声称与本次 Markdown 相同。

## 1. Gather / Scatter：不是有效载荷带宽，也没有发现写放大

### 原始报告重新算账

源代码：`src/integration/vllm_kv_cuda.cu`；调用和输入布局：`tools/kvf_cuda_bench.cu`、`integrations/vllm/kvfabric_vllm/cuda_pages.py`。

| 原始 ncu 单次 launch | Gather | Scatter |
|---|---:|---:|
| 有效载荷 P，byte | 75,497,472 | 75,497,472 |
| `gpu__time_duration.sum`，µs | 590.240 | 679.104 |
| `dram__bytes_read.sum`，byte | 75,514,496 | 75,513,984 |
| `dram__bytes_write.sum`，byte | 60,741,632 | 61,324,544 |
| **物理 DRAM 总带宽 `(R+W)/t`，GiB/s** | **214.9946** | **187.6602** |
| **单向有效载荷 `P/t`，GiB/s** | **119.1253** | **103.5372** |
| 逻辑读写带宽 `2P/t`，GiB/s | 238.2505 | 207.0743 |
| 原始 DRAM utilization | 90.258455% | 90.038389% |
| 实测 DRAM clock，GHz | 7.992627 | 6.993497 |
| 该时钟下 128-bit DDR 峰值，GiB/s | 238.1988 | 208.4225 |
| `l1tex__t_sectors_pipe_lsu_mem_global_op_st.sum` | 2,359,296 | 2,359,296 |
| store 数据/sector | 32 B，100% | 32 B，100% |
| load 数据/sector | 29.885714 B，93.3929% | 同左 |

来源：`native-linux-final/ncu-root/{gather,scatter}.csv` 与对应 `.ncu-repz`；原始十进制 Gbyte/s 先换算为 byte/s，再除以 `2^30`。完整复算输出为 `ncu-audit.json`。每核仅一个 launch、37 次 replay，不能描述成多样本稳定性结论。

**Scatter 的矛盾来自分母。** `187.6602 / 208.4225 = 90.0384%`；把它除以另一次 8 GHz 条件下的约 238.4 GiB/s，才会得到约 78.7%。原报告证明了显存时钟不同，不能证明是温度、功耗或某个驱动 P-state 导致，因而不称作已定位的“热降频”。Nsight 的时钟控制及 replay/cache 行为需按其定义理解：[NVIDIA Profiling Guide](https://docs.nvidia.com/nsight-compute/ProfilingGuide/)、[CLI Clock Control](https://docs.nvidia.com/nsight-compute/NsightComputeCli/)。

**没有支持非合并写或 RMW 放大的证据。** `2,359,296 × 32 B = 72 MiB`，恰好等于载荷；DRAM 读取仅比载荷多约 16–17 KiB，不存在额外读取一个载荷量的现象。代码以相邻线程访问相邻 `uint4`，Python 边界检查基址/stride 的 16B 对齐及末维连续。原始 DRAM 写入量反而小于载荷，不能把差值解释成写放大；本次计数没有独立区分 L2 驻留、写回采样窗口等因素，不为它们编造占比。

本轮重新采集相同核心计数，Gather / Scatter 分别为物理总带宽 **215.9594 / 187.4977 GiB/s**、单向有效 **119.5531 / 103.6153 GiB/s**、DRAM **90.6638% / 89.9653%**，显存仍约 7.993 / 6.993 GHz，store sectors 仍精确等于载荷。原始文件在本轮 `ncu/`。未发现值得修改的写合并问题，因此没有盲改 kernel。

面试可直接表述：

> 72 MiB 页打包微基准中，Gather / Scatter 的单向有效载荷带宽为 119.13 / 103.54 GiB/s；ncu 物理 DRAM 读写总带宽为 214.99 / 187.66 GiB/s，对应实际约 8 / 7 GHz 显存时钟下的 90.26% / 90.04% 利用率。两者 store sector 均为 100%，没有观察到写放大。这里的 kernel 带宽不代表 PCIe 或端到端 TTFT。

改动：`summarize_native_ncu.py` 现在明确区分 physical / payload / logical read+write，导出读写字节和 store sectors，并从日志读取 replay 次数；不再硬编码所有报告都是 37 passes。`ncu-parser-validation.json` 保存了对原归档的复算校验。

## 2. NVMe：1.719 s 的主要问题是逐层 QD≈1，不是 72 MiB 的裸盘读取

### 数据量与几何

实际模型配置为 36 层、2 个 KV heads、head_dim=128、BF16：

```text
每 Token KV = 36 × 2(K/V) × 2(KV heads) × 128 × 2 byte = 36,864 byte
8K 前缀 KV = 8,192 × 36,864 = 301,989,888 byte = 288 MiB
每层每页 = 16 tokens × 2 × 2 × 128 × 2 = 16 KiB
记录数 = 512 pages × 36 layers = 18,432
O_DIRECT 文件读取 = 18,432 × (16 KiB payload + 4 KiB header) = 360 MiB
```

`tools/kvf_nvme_recovery_bench.c` 中的 **72 MiB** 是独立的 `144 × 512 KiB` 顺序迁移微基准；其 **58.826840 ms / 1.195245 GiB/s** 包含 CRC、staging、H2D 和同步，不是裸 NVMe 带宽。vLLM 的 288 MiB 回传又分成四个 72 MiB staging 窗口，但每个窗口仍拆为 4,608 条 16 KiB 文件记录；不能把大块微基准当作这些小记录的恢复耗时。

### 历史差值与本次逐请求归因

历史 `5.005882409 − 3.287307012 = 1.718575397 s` 是两个独立样本集合的 P50 之差，原归档没有对应 NVMe 的完整分段 trace。**它本身没有可还原的逐段账本。** 本次对相同输入重新采集 CPU 和 SSD 路径；不能把新测耗时冒充旧 1.719 s 的组成。

以下是相同输入的第 2 条请求，所有值来自 NVTX / CUDA activity 或现有内部计时器，单位 ms：

| 项目 | CPU 热回源 | 旧 SSD 路径 |
|---|---:|---:|
| TTFT | 3269.508107 | 4817.475579 |
| fetch 墙钟区间并集 | 37.984244 | 1602.314211 |
| H2D 提交＋scatter＋同步 | 29.951270 | 30.633208 |
| 其他 TTFT（模型、调度等剩余） | 3201.572593 | 3184.528160 |

逐项差值精确闭合：**1547.967472 = 1564.329967 + 0.681938 − 17.044433 ms**。这次额外时间由 fetch 主导，而不是 H2D。

旧 SSD 的 1602.314211 ms fetch 可进一步按不重叠墙钟区间闭合：

| fetch 内覆盖区间 | ms |
|---|---:|
| 18,432 次文件读取 API（含等待完成） | 1320.435940 |
| 磁盘记录 CRC 校验 | 45.625596 |
| 文件 buffer → Host 目标 staging | 23.282328 |
| Segment 槽获取 | 2.814729 |
| 导入后的 Bridge staging / lease 操作 | 29.219291 |
| 剩余 fetch（网络/协议/元数据/未细分校验等） | 180.936327 |
| 合计 | **1602.314211** |

另有嵌套内部计时：接收/导入侧 CRC **30.560916 ms**、CPU staging **16.093732 ms**、commit **5.630751 ms**、目录 **11.590811 ms**、lease finish **7.077784 ms**、prefetch 排队 **1.144070 ms**、完成到 waiter 返回 **1.530485 ms**。这些数字与上表存在嵌套，**不能再次相加**。发送端所有 manifest/frame CRC 没有单独计时，因此不伪造“全路径 CRC 总耗时”。

OSRT 阈值为 1 µs；该请求没有记录到超过阈值的相关 `pthread_mutex_lock` 调用，其他两条旧 SSD 请求合计只记录 0.009861 ms。不能把未记录解释成精确零竞争。Segment 获取也不等于阻塞锁等待；completion-to-wait-return 含唤醒、重新拿锁和返回操作，不能称作纯调度延迟。现有证据不支持“锁或线程唤醒吞掉了秒级时间”。

CRC 代码已经按 SSE4.2 能力分派至三路硬件实现，见 `third_party/infer-io/src/base/crc32c.c`；没有将它误归因为 Python 逐字节校验，也没有以跳过 CRC 换取速度。

完整三请求逐项数据、嵌套计时、I/O 并发度与每请求 **288 MiB H2D 硬断言**见 `attribution.json`；`nsys-before-diagnostic/trace.{nsys-rep,sqlite}` 是主要归因证据。

### 修复与实测

原实现虽创建 4 个 fixed-buffer Ring，但每条记录都是提交一个请求后立即等 CQE；同一读取调用链只有一个 outstanding I/O。SSD 又被 `kvf_vllm_load_owned()` 的 `w->disk` 条件排除，只能经过本机 loopback 导入与额外 staging。旧路径观测到的并发文件读取 API 峰值为 **1**。

本次 patch：

1. 本机已发布 SSD 页复用原有 owned snapshot 入口；保留 export 可见性、引用 pin、generation/version、磁盘 CRC 与 manifest CRC，不将未经校验的页交给 GPU。
2. 每个 staging 窗口使用最多 `io_depth` 个有界读取线程，彼此处理独立页；全部 join 后才返回连续成功前缀，失败后缀继续走既有回退。
3. Direct 后端额外保留一个槽容量给单个后台 publisher，总槽数仍不超过 64。初版只用四槽时出现 51 次 save failure，已拒绝该组性能结果；增加容量后，并发读写回归与最终端到端测试均无 save error。
4. 增加磁盘读取、CRC、staging、槽获取和 join 的 NVTX；Benchmark 增加 `--require-cache-write-success`，写回出错时保留原始行并以失败退出。

最终 trace 三请求 fetch 为 **379.386 / 387.136 / 431.159 ms**，并发读取 API 峰值均 **4**。例如第 2 条：I/O 各线程累计 **1416.959195 ms**，其墙钟并集只有 **386.414629 ms**；磁盘 CRC 各线程累计 **43.800730 ms**，与其他 lane 的 I/O 重叠。不能把这些线程累计时间加到 387.135905 ms fetch 上。

**仍然按窗口读完再 H2D，且 H2D 与依赖它的 Prefill 不重叠。** 本 patch 实现的是多 lane 之间读取/校验的重叠；没有宣称实现完整的读－CRC－H2D 三阶段流水线。单独进一步改双 staging 异步交接，需要追加所有权及失败路径验证，未计入本次收益。

不启动 nsys 的同参数复测如下，两组均真实外部命中 50%，各自 15 条输出与同一冷算参考严格一致：

| 16K/8K hit/BF16/单并发 | 修前 | 最终修后 |
|---|---:|---:|
| TTFT P50，s | 4.961137 | **3.675437** |
| TTFT P95/P99，s | 5.077427 | **3.717917** |
| req/s | 0.173230 | **0.221237** |
| load / save error | 0 / 0 | **0 / 0** |
| best-effort save_skipped | 38 | 40 |

P50 降 **25.9154%**，P95 降 **26.7756%**，请求吞吐增 **27.7128%**。n=15 的 nearest-rank P95/P99 都是最大值，不当作可靠的生产尾延迟 SLA。两组顺序运行，未做随机化多轮交叉，变化归因于 **owned SSD 直读＋有界并发** 的组合，不归因于 fixed buffer。跳过的后台缓存写回不等于完整持久化成功。

原始数据：`matrix-before/kvfabric-disk/`、`matrix-final/kvfabric-disk/`。最终构建 23/23 CTest；真实 fixed I/O 并发 owned 读取、后台发布、失败前缀、零超时、持久记录 CRC 损坏回归通过。完整 vLLM 冷/热/部分命中/超时回退另保存在 `connector-final/`，不混入性能计时。

### O_DIRECT 与 io_uring fixed 的收益要分开

新增 `tools/kvf_io_compare.c` 复用生产 `mi_uring_transfer_fixed`，比较同一 4 KiB header + 16 KiB payload 几何、相同 CRC、相同确定性偏移：160 MiB 私有 ext4 文件，每请求 8 MiB payload，每格 200 请求，C=1/4，正反顺序各一轮。每格开始只对自己的文件 `fsync + POSIX_FADV_DONTNEED`；`mincore` 确认初始缓存为 0。没有清全局 Page Cache，也没清 SSD 控制器缓存。

| C=4，8 MiB 请求 | Direct POSIX r0 | fixed r0 | Direct POSIX r1 | fixed r1 |
|---|---:|---:|---:|---:|
| P50，ms | 40.817 | 39.741 | 40.011 | 40.502 |
| P95，ms | 42.221 | 41.354 | 42.205 | 42.386 |
| P99，ms | 44.839 | 41.924 | 42.723 | 42.752 |
| 200 请求进程 CPU sys，s | 0.482673 | 0.398906 | 0.389362 | 0.364067 |

结论：fixed 在这两轮 C4 的进程 sys 较低，但尾延迟改善不稳定；C1 的 sys 方向也随轮次变化。不将它写成确定的 CPU 或尾延迟收益，尤其不把 **5.006 vs 5.034 s** 的历史差距包装为加速。

确定的缓存结果是：**buffered POSIX 将该 160 MiB 文件的 resident Page Cache 从 0 增至 160 MiB；两种 O_DIRECT 路径始终为 0**。这是 O_DIRECT 的贡献，不能归给 fixed。各组进程 RSS 约 5.36–5.78 MiB，buffered 也没有随文件增长；文件 Page Cache 不计入普通 `read()` 进程 RSS，不能拿 RSS 证明“消除了系统缓存污染”。`mincore` 只说明测试文件占用，未证明实际挤出了模型页或改善了生产 TTFT。

buffered 组之后会命中热 Page Cache，P50 约 1.35–1.48 ms；它不是每请求冷盘对照。尾延迟是闭环 I/O 请求服务时间，不含排队准入，不是 LLM TTFT。CPU sys 使用 `getrusage(RUSAGE_SELF)`，不代表全系统内核/中断 CPU，不能据此推出系统级 CPU 节省。所有明细见 `io-summary.json` 和 `io-r*-c*-*.{csv,json}`。

## 3. RDMA：独立保留跨 Worker 数据面成果，但不捏造硬件加速

### 实现定位

RDMA 是 **MiniInfer Storage / Nano-VLLM 跨 Worker KV 数据面**，不是 NVMe 的附属功能。目录注册、查询和租约校验仍走 TCP；数据面通过 `rsocket/rconnect/raccept/rsend/rrecv/rpoll/rclose` 提供双边字节流，复用 Wire V3、batch、CRC、身份、generation/version 和 lease 协议。连接池按 endpoint、Boot UUID 和 transport 隔离；严格模式禁止 TCP 回退，实际传输后检查 `active_transport`。

CUDA 使用 Host staging 后再 H2D/D2H。它不是应用层单边 RDMA READ/WRITE，也不是 GPUDirect RDMA。vLLM Bridge 的 `kvf_vllm_open()` 当前没有传输选择配置，3B 的 CPU/SSD TTFT 测试没有经过 rsockets；不能把该 TTFT 收益归于 RDMA。

### 已有端到端证据

历史 `rdma/{cpu,cuda}-paired.csv` 来自另一台 WSL2 / RTX 5060 Ti / stories15M 环境，不能和本机 4060 / Qwen3B 混用。每一负载分别重算 nearest-rank 分位，不能把两个 payload 混池：

| 设备 / 实际复用 | TCP TTFT P50，ms | RXE TTFT P50，ms |
|---|---:|---:|
| CPU / 20 Token，276,480 B | 32.977395 | 36.223601 |
| CPU / 120 Token，1,658,880 B | 60.152960 | 67.811233 |
| CUDA / 20 Token | 11.144785 | 14.032141 |
| CUDA / 120 Token | 37.019429 | 42.772048 |

两组 prompt 分别为 24 / **124** Token，因此 120/124 的远程复用占 **96.77%**；它说明减少的 Prefill 工作量，不是 TTFT 下降比例。生成 Token 及 KV 页摘要与冷算一致。RDMA 在四组都慢于 TCP；历史冷算仅作为正确性参考，没有足够原始冷算计时支撑“RDMA 比冷启动快 X%”。

已连接断链与 500 ms deadline 故障记录在 `rdma/fault-recovery.json`：暂停响应时网络接收 **498.316007 ms** 后本地 Prefill，断链场景丢弃未验证后缀，并能在源恢复后重新导入。单次故障记录不是尾延迟 SLA，也不能将 deadline 写成整个请求 TTFT。

### 本轮原生 Linux RXE 重测

在任务专用 veth + RXE 上使用生产 Worker CLI，CPU/CUDA、256 KiB / 4 MiB / 16 MiB，TCP/RDMA 交替顺序，各 30 对，逐字节校验。`TRANSFER` 时窗覆盖目录、建连、source snapshot、CRC、接收及目标提交；排除进程启动和计时后的文件输出，**不是裸链路带宽**。每次新建 consumer，因此该组含连接获取，没有伪称热连接复用吞吐。

MTU=1500 时，16 MiB 的 TCP / RXE 有效载荷带宽为 CPU **1.2402 / 0.4485 GiB/s**、CUDA **1.4867 / 0.4332 GiB/s**。全部原始日志、CSV 和分位在 `rdma-payload-final/`；另外严格 CPU/CUDA 功能回传均已通过，见 `rdma-strict-final/`。

MTU=9000（RXE active_mtu=4096）复测如下。MTU 是本轮主动验证的配置优化，不是 kernel 或 rsockets 的代码修改。

| 16 MiB payload | MTU 1500 TCP | MTU 1500 RXE | MTU 9000 TCP | MTU 9000 RXE |
|---|---:|---:|---:|---:|
| CPU P50，ms | 12.599 | 34.840 | 12.852 | 30.706 |
| CUDA P50，ms | 10.510 | 36.067 | 11.056 | 30.195 |
| CPU RXE 有效 GiB/s | — | 0.448479 | — | 0.508858 |
| CUDA RXE 有效 GiB/s | — | 0.433222 | — | 0.517470 |

MTU 9000 的全部三种载荷、两种设备、各 30 对原始行在 `rdma-payload-mtu9000/`。调回 1500 后，16 MiB RXE P50 回到 CPU **36.141 ms**、CUDA **36.848 ms**，见 `rdma-payload-mtu1500-recheck/`。这一 A→B→A 检查支持大 MTU 的改善效果；改善没有使 RXE 超过 TCP，也不能据此断言瓶颈完全来自包处理，未采集全内核 CPU/packet profile。本机结果不能外推为物理 HCA 的带宽或跨机 SLA。测试后专用 RXE/veth 与本轮加载的 RXE 模块已清理，状态记录在 `rdma-cleanup.txt`。

**简历保留策略**：单独一条写“跨 Worker KV 数据面与一致性/故障恢复”，量化实际跳过的 Prefill、有效载荷与 batch；明确 Soft-RoCE。将 RXE/TCP 的完整性能表和局限放 README。现有证据不足以写“RDMA 显著加速 Qwen3B TTFT”或“达到物理 RoCE 链路峰值”。

## 4. 复现入口与验证范围

下列命令从仓库根目录运行。模型、venv、NVMe 目录使用真实存在的本地路径；其他机器需替换，并保留环境与模型 SHA256。所有性能任务串行，避免共享 GPU/SSD 争用。

```bash
export PATH="$PWD/.kvfabric-local/native-linux-final/20261125T153218+0800/venv/bin:$PATH"
export VLLM_BATCH_INVARIANT=1 VLLM_NO_USAGE_STATS=1
MODEL="$PWD/.kvfabric-local/native-linux-final/20261125T153218+0800/models/Qwen2.5-3B-Instruct"
DISK="$PWD/.kvfabric-local/native-linux-final/20261125T153218+0800/disk"
OUT="$PWD/docs/benchmarks/perf-attribution-rerun"
AFTER="$PWD/.kvfabric-local/perf-attribution/build-rerun-after"
BEFORE="$PWD/.kvfabric-local/perf-attribution/build-rerun-before"

cmake -S . -B "$AFTER" -DCMAKE_BUILD_TYPE=Release \
  -DMINIINFER_WITH_CUDA=ON -DMINIINFER_WITH_URING=ON \
  -DMINIINFER_BUILD_VLLM_BRIDGE=ON -DMINIINFER_VLLM_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build "$AFTER" -j8
python3 scripts/build_perf_baseline.py \
  "$PWD/.kvfabric-local/perf-attribution/source-rerun-before" "$BEFORE"

gcc -O3 -Wall -Wextra -Wpedantic -DMINIINFER_WITH_URING \
  -Iinclude -Isrc/worker tools/kvf_io_compare.c \
  -L"$AFTER/src/integration" -lkvf_vllm \
  -Wl,-rpath,"$AFTER/src/integration" -pthread \
  -o "$PWD/.kvfabric-local/perf-attribution/kvf_io_compare-rerun"

python3 scripts/run_perf_attribution.py "$OUT" --model "$MODEL" --disk-dir "$DISK" \
  --before "$BEFORE" --after "$AFTER" \
  --io-bench "$PWD/.kvfabric-local/perf-attribution/kvf_io_compare-rerun"
```

完整本地归档保留实际执行 argv、环境和失败日志。公开 `excluded-runs.json` 保留被拒用实验的原因；公开测量未包含个人路径、主机标识或凭据。上面的参数化命令用于新环境复现，不冒充原始环境记录。

```bash
# ncu：导出同时含读写字节、时钟和 store sectors 的原始报告。
scripts/profile_kv.sh bench "$AFTER" "$MODEL" "$OUT/ncu-root"
python3 scripts/summarize_native_ncu.py --archive "$OUT"

# 完整 GPU 正确性，包括超时回退；与性能测试分开。
python3 tests/vllm/test_connector_roundtrip.py --model "$MODEL" \
  --library "$AFTER/src/integration/libkvf_vllm.so" --directory "$AFTER/bin/kvfabricd" \
  --out "$OUT/connector" --require-uring-fixed

# 专用临时网络；setup/cleanup 需要主机 root，仅操作脚本自有设备。
sudo bash scripts/perf_softroce_setup.sh setup
python3 scripts/bench_rdma_payload.py --build "$AFTER" --address 198.19.77.1 \
  --out "$OUT/rdma-1500" --rounds 30
sudo bash scripts/perf_softroce_setup.sh mtu 9000
python3 scripts/bench_rdma_payload.py --build "$AFTER" --address 198.19.77.1 \
  --out "$OUT/rdma-9000" --rounds 30
python3 tests/worker/test_rdma_roundtrip.py --build "$AFTER" --address 198.19.77.1 \
  --out "$OUT/rdma-strict" --rounds 30
sudo bash scripts/perf_softroce_setup.sh cleanup
```

未完成/不扩大声明：没有双机物理 HCA、GPUDirect、Qwen3B 经 RDMA 的 TTFT 或生产高并发 SLA；没有通过这些结果证明硬件永久故障在线恢复或全部 KV 必定持久化。未来要写物理 RDMA 加速，需要双机冷/热连接、并发/大载荷、CPU 与网络端口计数、同负载冷算 TTFT 和完整 Token/KV 校验的配对证据。


公开附件说明：文中的 `.nsys-rep`、`.ncu-repz`、SQLite、环境及服务日志路径是本地源归档位置。公开仓库提供提取的数字与逐请求数据，详见证据目录 README；白名单导出脚本为 `scripts/export_perf_publication.py`，没有上传完整源报告或私人工作区备份。
