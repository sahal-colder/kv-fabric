# 三星原盘 SPDK / io_uring 实测对照

验收日期：2026-11-28，Australia/Perth。正式运行：10:56:23–10:58:08。

后续已完成[异步流水线优化与独立复测](SPDK-PIPELINE-OPTIMIZATION.md)：同 QD4 的 SPDK 回载延迟降低约 25%。本页保留首次分阶段对照的原始结论；新结果没有覆盖历史样本。

**结论：本机没有可用于简历的“SPDK 全面加速”结论。** 同盘、同负载时，QD=1 的 GPU 回载 P50 仅改善 0.84%，P95 改善在批次间变号；两边各自最优配置均为 QD=4，此时 SPDK 的 GPU 回载 P50/P95 分别慢 2.37%/2.96%，计时窗口内进程 CPU 时间约为 io_uring 的 4.69 倍。

确定性小模型验证了 **SPDK SSD 卸载本身减少前缀重算**，但引擎的 io_uring 路径尚未接通同盘裸设备，**SPDK 相比 io_uring 的额外 TTFT 改善未测得、未证明**。下面将后端回载实验与推理实验分别交付。

## 1. 环境、身份与实验边界

| 项目 | 本次实际状态 |
|---|---|
| 主机 | 原生 Ubuntu，Linux 6.8.0-149-generic，Intel i7-13700H，单 NUMA 节点 |
| GPU | RTX 4060 Laptop，8 GiB；驱动 615.71.09，CUDA 13.4.92 |
| SSD | SAMSUNG MZVLQ512HALU-00000，序列号 `S4Y4NX0R755035`，512110190592 字节 |
| 设备 | PCI `0000:07:00.0`，独占 IOMMU 组 16，PCIe 8.0 GT/s ×4 |
| SPDK | 运行日志 v27.01-pre，git `4828014`，DPDK 25.11.0；PCIe bdev `Samsungn1`，vfio-pci，IOVA=VA，基址 `0x1000000000` |
| io_uring | liburing 2.9，内核 nvme，按序列号重新寻找 `/dev/nvme0n1`；裸设备 `O_RDONLY|O_DIRECT`，固定注册缓冲区，无 SQPOLL |
| 编译器 | GCC/G++ 11.4.0，Release，CUDA 架构 89 |
| CPU 亲和性 | 基准调用线程 CPU 4；SPDK reactor CPU 2；两者为不同物理核。推理也使用同样安排 |
| 电源与温度 | CPU governor 保持开测前的 powersave；未锁 CPU/GPU 频率。正式样本 SSD 30–57°C，逐轮保留温度 |
| 开测前 / 结束后 | `vfio-pci`；服务 active/exited、enabled。结束后完全恢复此状态 |
| 系统盘 | UMIS 1 TiB，另一个 PCI 设备，保持原驱动与挂载；未对其作裸设备写入 |

源码基准提交为 `3af663a`，其父提交为 `55e3142`。开测时工作区已有大量未提交依赖改动，所以**仅凭提交号不足以复现本次二进制**。本交付同时保存了实际工作树的[源码包](benchmarks/spdk-uring-2026-11-28/measured-source.tar.gz)、逐文件/二进制 [SHA-256](benchmarks/spdk-uring-2026-11-28/source-sha256.json)、构建配置及完整构建日志；源码包已在独立目录重新构建成功。没有把用户已有改动混入本次提交。

完整环境见 [environment.txt](benchmarks/spdk-uring-2026-11-28/environment.txt) 与[补充环境](benchmarks/spdk-uring-2026-11-28/environment-supplement.txt)。环境日志中的 `nvme id-ctrl/smart-log` 因未安装 nvme-cli 未执行成功；身份、容量通过 sysfs 核实，温度通过 sysfs/SPDK NVMe GET LOG PAGE 采集。`pkg-config spdk_event` 的 16.0 是包版本字段，本报告采用实际运行日志中的 SDK 版本。

## 2. 同负载方法与实际队列深度

本次比较的是两条独立读盘路径，分别启动进程、切换设备驱动后运行：

| 测试组 | SSD 数据读取路径 |
|---|---|
| io_uring 基线 | 基准程序 → io_uring → Linux 块层 → 内核 nvme 驱动 → 三星 NVMe |
| SPDK | 基准程序 → SPDK PCIe NVMe bdev / 用户态 NVMe 驱动 → 三星 NVMe |

**SPDK 组直接绕过 Linux 块层及内核 nvme 驱动，没有在 io_uring 上叠加 SPDK。** 该组将控制器绑定到 vfio-pci，由 SPDK 经 VFIO/IOMMU 建立 DMA 映射并在用户态提交、轮询 NVMe I/O；VFIO 提供设备访问和映射，不是内核块设备读盘路径。配置使用 `bdev_nvme_attach_controller`、`trtype=PCIe`、`traddr=0000:07:00.0`，没有使用 SPDK 的文件、AIO 或 uring bdev。

代码中 SPDK 分支调用 `spdk_bdev_read`；io_uring 分支才创建 ring 并调用 `io_uring_prep_read_fixed`，二者互斥。后文数值中的“基线 → SPDK”表示两个独立测试组的结果对照，不表示 I/O 调用链。

测试区固定为 `[1073741824, 1207959552)`，即起点 1 GiB、长度 128 MiB。主对照开始前经裸设备 `O_DIRECT` 校验原始区域与备份完全相同，然后仅写入一次固定记录集，后续四批、两后端、两 QD、两目标全部读取同一批记录。

每轮 32 个 KV 记录，每个含 4096 字节头区及 524288 字节有效载荷，步长 528384 字节。有效载荷为 **16777216 字节（16 MiB）**，实际读盘 16908288 字节（16.125 MiB）；起始偏移为 `1 GiB + i × 528384`。头部含 magic、格式版本 1、ID `i+1`、数据版本 1、长度及 CRC32C。载荷模式固定，顺序固定为 0–31。

两后端共用 [kvf_backend_compare.c](../tools/kvf_backend_compare.c) 中的轮次、校验及 H2D 代码。主机缓冲区为相同地址策略的 18 MiB、2 MiB hugepage 映射，`mlock` 并注册到 CUDA；SPDK 另做地址翻译注册，io_uring 另做 fixed-buffer 注册。两边均直接读到该缓冲区。初始化、缓冲区分配/注册、GPU 分配和清零均在计时外。

GPU 计时窗口为：发出首批读取（SPDK 包括向 reactor 交接）→全部读取完成→逐记录核对身份/版本/长度并计算 CRC32C→同一 CUDA stream 上提交 32 次、每次 512 KiB H2D→最后一次 stream 同步完成。CRC **包含在主指标内**。不重叠读取与 H2D。CPU 目标采用相同读取和 CRC，只省略 H2D。每轮计时后，另对全部记录逐字节比较；GPU 目标还 D2H 回读整个 16 MiB 并逐块比较。

QD=1/4 是**真正异步提交且限制同时未完成的逻辑记录请求数**：SPDK 按 bdev 回调完成后补交，io_uring 首次一起 submit 1/4 个 SQE、按 CQE 补交；每轮检查 `max_inflight == qd`、submitted=completed=32。所有 1056 个含预热轮次均通过。它们不是“只配置 ring 大小为 4、实际串行”。这里的 QD 是 bdev/SQE 层逻辑请求深度，**不等于硬件 NVMe command 深度**：内核 `max_sectors_kb=128`，528384 字节逻辑请求可在两条路径中进一步拆分；本轮未跟踪控制器级 command 深度。

历史 `kvf_spdk_verify` 则逐次等待迁移完成，其实际 QD=1。新基准绕过了生产分层存储的逐块迁移、额外拷贝和元数据流程，因此本次衡量的是**带 KV 校验及 H2D 的后端回载性能**，不能把历史 18.95 ms 与新基准的 9.89 ms 相除当作后端收益。

共四批，每批每组预热 3 轮、正式 30 轮。顺序为：

| 批次 | 后端顺序 | 每个后端内部顺序 |
|---|---|---|
| 1 | 先 io_uring，后 SPDK | GPU QD1、GPU QD4、CPU QD1、CPU QD4 |
| 2 | 先 SPDK，后 io_uring | CPU QD4、CPU QD1、GPU QD4、GPU QD1 |
| 3 | 先 io_uring，后 SPDK | 同批 1 |
| 4 | 先 SPDK，后 io_uring | 同批 2 |

每组汇总 **120 个正式样本**；全部保留，共 960 个正式、96 个预热。P50 是中位数，P95 是 nearest-rank。各批次的 P95 用第 29/30 个样本，合并 P95 用第 114/120 个样本。吞吐以 16 MiB 有效载荷计算，未包含头区，不使用实际读盘字节夸大有效吞吐。温度在计时外逐轮读取，以整数 °C 保存；内核毫摄氏取整与 SPDK 的 Kelvin−273 换算可能相差约 1°C。

## 3. GPU 主指标：io_uring 基线与独立 SPDK 路径对照

“改善”按 `(io_uring − SPDK) / io_uring × 100%`，正数代表 SPDK 延迟较低。

| 实际逻辑 QD | GPU 可用 P50，ms | P50 改善 | GPU 可用 P95，ms | P95 改善 |
|---|---:|---:|---:|---:|
| 1 | **13.098071 → 12.987448** | +0.84% | **13.649153 → 13.313202** | +2.46% |
| 4 | **9.661468 → 9.890206** | **−2.37%** | **10.086211 → 10.385186** | **−2.96%** |

| QD | io_uring 有效 GiB/s @P50 / @P95 | SPDK 有效 GiB/s @P50 / @P95 |
|---|---:|---:|
| 1 | 1.192924 / 1.144760 | 1.203085 / 1.173647 |
| 4 | 1.617249 / 1.549145 | 1.579846 / 1.504547 |

两者按 P50 选择最优配置均为 **QD=4**，所以最优配置对照仍为 **9.661468 → 9.890206 ms**，SPDK 慢 0.228738 ms（2.37%）。不能用 SPDK QD4 对 io_uring QD1 表述后端加速。

### 跨批次复现与 CPU 成本

| 批次 | QD1 P50：基线 → SPDK，ms | P50 改善 | QD1 P95 改善 | QD4 P50：基线 → SPDK，ms | P50 改善 | QD4 P95 改善 |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 13.079 → 13.037 | +0.32% | −0.63% | 9.624 → 9.927 | −3.15% | −4.27% |
| 2 | 13.121 → 12.882 | +1.83% | +3.08% | 9.669 → 9.828 | −1.64% | −1.98% |
| 3 | 13.094 → 13.040 | +0.41% | +2.42% | 9.729 → 9.869 | −1.45% | −0.61% |
| 4 | 13.201 → 12.973 | +1.73% | +5.87% | 9.664 → 9.904 | −2.49% | −3.70% |

QD1 的 P50 小幅改善在四批中方向一致，范围 **0.32%–1.83%**，但 P95 为 **−0.63%–+5.87%**，不能宣称尾延迟稳定改善。QD4 四批中 P50/P95 均较慢，P50 慢 **1.45%–3.15%**，P95 慢 **0.61%–4.27%**。这里是这台机器、当前频率策略、这组固定热记录的观测，不是置信区间，也不是所有 SSD/负载的普遍结论。

使用 `CLOCK_PROCESS_CPUTIME_ID`，统计整个进程所有线程在主计时窗口内消耗的 CPU 时间，**包含 SPDK reactor 的忙轮询**，不局限于调用线程。以下不包括初始化和计时后的数据比对；轮询线程在窗口外仍可能耗 CPU，不能把这些数值理解为整个进程生命周期的总成本。

| GPU QD | 每轮 CPU 时间 P50：基线 → SPDK，ms | 多用 CPU ms | CPU 时间倍数 | 平均占用核数：基线 → SPDK |
|---|---:|---:|---:|---:|
| 1 | 3.060740 → 15.548007 | +12.487267 | 5.08× | 0.234 → 1.196 |
| 4 | 2.685538 → 12.605421 | +9.919883 | 4.69× | 0.281 → 1.258 |

平均核数为各样本 CPU 时间之和 / 延迟之和。120 个 GPU 正式样本的窗口内总 CPU 时间：QD1 为 0.37061 → 1.86152 秒，QD4 为 0.32888 → 1.49403 秒。此配置为 SPDK 额外安排一个 reactor 物理核，且没有启用中断休眠；本轮不代表经其他轮询策略调优后的最小 CPU 成本。

### SSD→CPU 和分段数据

| QD | CPU 可用 P50：基线 → SPDK，ms | P50 改善 | CPU 可用 P95：基线 → SPDK，ms | P95 改善 |
|---|---:|---:|---:|---:|
| 1 | 11.667795 → 11.400690 | +2.29% | 12.616359 → 11.855887 | +6.03% |
| 4 | 8.260900 → 8.411251 | −1.82% | 8.664638 → 8.993211 | −3.79% |

| CPU 目标 QD | 有效 GiB/s @P50：基线 → SPDK | 有效 GiB/s @P95：基线 → SPDK | 每轮 CPU 时间 P50：基线 → SPDK，ms |
|---|---:|---:|---:|
| 1 | 1.339156 → 1.370531 | 1.238471 → 1.317911 | 1.614772 → 11.986290 |
| 4 | 1.891440 → 1.857631 | 1.803307 → 1.737422 | 1.303829 → 9.121145 |

GPU 轮次分段的 P50 如下；各段中位数来自不同排序，不能要求它们严格相加等于总延迟的中位数。

| QD | 纯读取：基线 → SPDK，ms | 身份/版本/CRC：基线 → SPDK，ms | H2D+同步：基线 → SPDK，ms |
|---|---:|---:|---:|
| 1 | 10.720862 → 10.443268 | 0.965996 → 1.117911 | 1.393879 → 1.390264 |
| 4 | 7.295824 → 7.335831 | 0.921019 → 1.146739 | 1.394408 → 1.394174 |

QD1 纯读取节省约 0.278 ms，但后续校验抵消部分差异；QD4 纯读取接近，校验耗时差异超过读取差异，H2D 基本相同。SSD→CPU 也呈现 QD1 略快、QD4 略慢，因此不能把 GPU 端差异全部归因于 H2D。忙轮询与 CPU 频率/缓存可能影响校验耗时；本轮没有做关闭 CRC 或锁频的干预，**没有验证这一因果解释**。

### 原始样本核查

[汇总脚本](../scripts/summarize_spdk_uring_compare.py)检查：轮号连续、每文件 3+30 轮、每组 120 正式轮、有效载荷/校验字节相同、实际 QD 相同、32 次提交全部完成、温度有效、分段之和与总延迟一致；所有检查通过。每个进程另逐轮检查数据内容、身份、版本与 CRC。无删样、截尾、重排或按结果挑批。

[极端样本清单](benchmarks/spdk-uring-2026-11-28/sample-audit.json)保留了最长 10 个及最短 4 个正式轮次。例如 `b3-uring-qd1-cpu.csv` 第 9 轮为 17.630155 ms，其中读取 16.661089 ms；`b2-uring-qd1-gpu.csv` 第 2 轮为 17.100146 ms，其中读取 14.701289 ms。它们没有被剔除。基线偶发读取长尾会影响小样本 P95，这也是报告跨批次范围的原因。样本温度逐轮可追溯；未采集控制器热节流计数，因此不宣称已排除所有温度/动态频率影响。

历史代码本轮重测同样 3 预热+30 正式，SPDK GPU 回源 **P50 19.194180 ms、P95 23.475149 ms**，相较历史 18.951510 / 23.176393 ms，P50 高 1.28%。原始[历史重测 CSV](benchmarks/spdk-uring-2026-11-28/historical-cuda-replay.csv)与日志均保留。历史值的数量级已复现；这一步独立于前面的只读固定记录对照，在主对照完成后才执行其原有写入。

## 4. 推理 A→B→A：能证明什么，缺口在哪里

使用已有 `tests/nanovllm/test_remote_inference.py::fixtures(seed=7)`，dim=32、hidden=64、2 层、4 heads、2 KV heads、vocab=260、context=192。模型约 111 KiB，非 Qwen/生产模型。模型与 tokenizer 的 SHA-256 记录在 [run.json](benchmarks/spdk-uring-2026-11-28/run.json)。

请求固定为 A=`abcdefghijklmnopabcdefghijklmnop`，B=`ZYXWVUTSRQPONMLKZYXWVUTSRQPONMLK`，再 A；tokenizer 实际每个 prompt 为 34 Token。KV 池 8 页、每页 8 Token，容量 64 Token，约 16 KiB；B 淘汰 A 的首个完整页。batch=1、greedy `-t 0`、固定默认 seed=1、4 个输出 Token、ignore-eos。**关闭 SSD 卸载仍开启 GPU 前缀缓存**，仅 `ssd-slots=0`。

三批按 off→SPDK、SPDK→off、off→SPDK 交替；每配置每批 3 次预热、10 次正式，合计各 30 正式次。每次 A/B/A 都启动新进程，从空的 GPU 缓存和 SSD 前缀索引开始；盘上旧记录没有可命中的索引，随后按同一顺序重新写入。SPDK 使用同一三星盘、同一 1 GiB 测试区、生产 `mi_ssd_spdk_backend_create` / 同步桥接层，实际 QD=1。

TTFT 来自引擎 `run_prompts` 提交前的时间点至 `first_token_ns`（首 Token 采样完成可用），不含进程、模型、SPDK 初始化，不是 CLI 最后打印全部 Token ID 的时刻。使用现有 `--trace` 指标，无新增推理计时逻辑；两边都开启 trace，trace 的步间开销也包含在该端点时间里。小模型绝对值及百分比不可外推到生产负载。

| 配置 | 第三个 A 的正式样本数 | TTFT P50 / P95，ms | SSD 读记录次数 | 实际 SSD 恢复 Token | GPU 驻留复用 Token | 总复用 Token | 重新 Prefill Token |
|---|---:|---:|---:|---:|---:|---:|---:|
| 关闭 SSD 卸载 | 30 | **1.439362 / 1.585836** | 0 | 0 | 0 | 0 | 34 |
| io_uring SSD 卸载 | **无合格样本** | N/A | N/A | N/A | N/A | N/A | N/A |
| SPDK SSD 卸载 | 30 | **0.310557 / 0.448900** | **1** | **8** | **24** | **32** | **2** |

所有 78 个含预热的 A/B/A 输出 Token ID 严格一致；第三个 A 都是 **`143 34 147 0`**。每次 SPDK 第三个 A 的 SSD 读取增量确实为 1；该读盘记录经过版本/身份和 CRC 校验。这页 KV 有效数据仅 2048 字节，底层读 4096 字节对齐记录，远小于前面 16 MiB 微基准。读取 8 Token 的一页之后，原本驻留的 24 Token 后缀才能继续复用，故**减少重算 32 Token（34 → 2，减少 94.12%）不等于从盘恢复了 32 Token**。

SPDK 卸载对比关闭卸载，TTFT P50 **1.439362 → 0.310557 ms**，减少 1.128805 ms（78.42%）；P95 **1.585836 → 0.448900 ms**，减少 1.136936 ms（71.69%）。三批 P50 分别约 1.436→0.311、1.440→0.311、1.439→0.306 ms，前缀重算的节省可重复。

**这证明 SSD 前缀卸载的收益，不能证明 SPDK 相比 io_uring 的收益。** 当前 `cli.c::attach_ssd` 的 `--ssd-mode uring` 创建私有临时文件，`storage_backend.c::open_file` 会截断文件，且 `mi_uring_transfer` 为串行缓冲 I/O。它没有裸盘偏移/容量/O_DIRECT 适配；不能把 `/dev/nvme0n1` 填入文件构造器。为避免将文件系统/页缓存或系统盘结果冒充同盘 SSD 结果，本轮未接受该路径为第三组 TTFT 数据，也未修改生产引擎。前述微基准的裸设备适配已经实测，但尚未接入推理引擎。

因此，“io_uring 卸载节省多少 TTFT”和“SPDK 比 io_uring 额外改善多少 TTFT”均为 **N/A、尚无证据**，不能以 0 或 78.42% 代替。完成这一比较还需要有边界检查的裸设备 io_uring 推理后端，然后在同盘重复三配置；本交付不把该缺口隐藏在微基准结果中。历史 Qwen TTFT 的 25.9% 未参与任何计算。

推理全部原始日志为 `infer-b*-*.txt`，第三个 A 的逐轮[原始指标 CSV](benchmarks/spdk-uring-2026-11-28/inference.csv)含预热标志、读次数、恢复/复用/重算 Token、输出 ID 和 TTFT。

## 5. 原盘保护与恢复证据

开测前按序列号和同一 boot 的状态记录核对 PCI，切回内核后重新按 sysfs 序列号、型号、容量识别；检查无挂载/swap/holder/占用。核对原有首/尾各 1 MiB 和 128 MiB 测试区备份长度及 SHA-256，并通过裸设备 O_DIRECT 读取三处与备份比较。主对照、历史复测、小模型推理的所有裸盘写入均限制在原验收区。未格式化、分区、建文件系统，也未对 UMIS 系统盘作裸写；仓库日志和源码按正常文件方式保存。

脚本 `finally` 在正常结束、Python 异常或 SIGINT/SIGTERM 后执行恢复。它不能承诺在断电、SIGKILL 或恢复设备本身故障时自动完成；持久备份仍保留在原本的本地忽略目录，未入库。当前这次正常完成，恢复结果如下。

| 区域 | 偏移 / 长度（字节） | 原备份、开测前直接回读、恢复后直接回读 SHA-256 |
|---|---|---|
| 首部 | 0 / 1048576 | `5f80513ceef293b98d109e6a4746ef36ec28cf503159bf746dc28433999e9a15` |
| 测试区 | 1073741824 / 134217728 | `254bcc3fc4f27172636df4bf32de9f107f620d559b20d760197e452b97453917` |
| 尾部 | 512109142016 / 1048576 | `30e14955ebf1352266dc2ff8067e68104607e750abb9d3b36582b8af909fcb58` |

恢复使用 O_DIRECT 写回完整 128 MiB，fsync、flush block buffers 后，再通过 O_DIRECT 独立回读核验三处 SHA-256。随后恢复 `vfio-pci` 绑定，以及服务 active/exited、enabled；未更改 enable 状态。具体前后检查值、驱动和服务状态在 [run.json](benchmarks/spdk-uring-2026-11-28/run.json) 的 `checks`、`original_state`、`final_state`。此前独立冒烟测试同样恢复核验成功，其结果不混入正式样本。

## 6. 复现与交付索引

先在同一验收机确认原备份仍在 `.kvfabric-local/spdk-samsung/`，不要以结果目录中的摘要代替实际备份。执行脚本只接受本次已核实的三星序列号和原区域，目标不一致会终止。脚本的 `prepare` 会写固定记录，原盘操作请使用完整 runner。

```bash
cmake -S tools/backend_compare -B build-backend-compare \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=89 \
  -DNANO_WITH_CUDA=ON -DMINIINFER_BUILD_VLLM_BRIDGE=ON \
  -DMINIINFER_VLLM_CUDA=ON
cmake --build build-backend-compare \
  --target kvf_backend_compare miniinfer kvf_spdk_verify -j6

# OUT 必须是一个尚不存在的新目录；本轮正式目录的完整 argv 在 run.json。
sudo python3 scripts/run_spdk_uring_compare.py \
  --out docs/benchmarks/spdk-uring-new-run --batches 4
python3 scripts/summarize_spdk_uring_compare.py \
  docs/benchmarks/spdk-uring-new-run

# 不接触设备，仅重算本次报告并检查原始样本。
python3 scripts/summarize_spdk_uring_compare.py \
  docs/benchmarks/spdk-uring-2026-11-28
```

如当前工作区已有变化，在空目录解开 `measured-source.tar.gz` 后执行上面的构建命令，可取得本次依赖源码；[独立重建日志](benchmarks/spdk-uring-2026-11-28/source-rebuild.txt)已证明该源码包可构建三个实测目标。SDK/CUDA/liburing、root 管理服务及原始备份属于验收机前置环境，没有打包到源码归档。

交付目录：[spdk-uring-2026-11-28](benchmarks/spdk-uring-2026-11-28/)。其中：

- `b{1..4}-{uring,spdk}-qd{1,4}-{cpu,gpu}.csv`：32 份逐轮回载原始样本，含预热、分段、CPU 时间、实际 QD、温度及校验字节。
- 同名 `.txt`：每个进程的原始 stdout/stderr 和完成校验标志；`run.json`：113 条实际命令及运行顺序、耗时、返回码、模型哈希与恢复证据。
- `summary.json`、`inference.csv`、`historical-cuda-replay.csv`、`sample-audit.json`：汇总及可核查派生数据。
- `environment*.txt`、`configure.txt`、`CMakeCache.txt`、`build*.txt`、`source-rebuild.txt`、`measured-source.tar.gz`、`source-sha256.json`：环境、准确源码与复现证据。

用户指定的 `scripts/test_spdk_nvme.py` 起初不在当前工作区；从此前发布源码快照中恢复了相同文件（SHA-256 `da0b621e856ff617aacc271dd38d6649cd411c90a16182ff2daac292b5a1669c`）并阅读核对。本次使用新的只读对照 runner，旧脚本作为历史验收工具保留。

## 7. 可直接放入中文简历的一句话

> 在原生 Ubuntu、三星 MZVLQ512HALU NVMe 与 RTX 4060 Laptop 上建立可复现的 16 MiB KV 回载对照，四批实测最优 QD=4 时，io_uring 基线的 GPU 可用延迟 P50/P95 为 9.66/10.09 ms，绕过内核块层的独立 SPDK 路径为 9.89/10.39 ms，未观察到 SPDK 稳定优势，完成原盘数据恢复与 SHA-256 回读核验。

面试时应补充：QD1 中位数有约 0.84% 的微小优势，但 P95 跨批次变号；QD4 更慢且 CPU 成本更高。小模型中省下 32 Token 的前缀重算属于 SSD 卸载效果，**不是已证明的 SPDK 对 io_uring 的推理收益**。
