# SPDK / io_uring 同盘性能与可靠性报告

日期：2026-11-28。以下均为生产实现的真实测量，不使用历史 backend_compare 的独立 SPDK 快路。Raw 测量后只优化了其不链接的记录层；Raw 源码已归档，实测 Raw 二进制 SHA 与最终编译产物完全一致。迁移及推理使用最终记录层重新测量。

实测结论：两后端在所有 Raw 配置都达到对应的实际 QD，最高 128。SPDK 在中高 QD 的大块读取和 GPU Recovery 有约一成收益，但没有小块峰值 IOPS 或 CPU 成本上的全面优势。高 QD 会增加尾延迟，不能只看带宽。

## 环境、数据语义与保护

CPU：Intel Core i7-13700H，单NUMA节点；GPU：RTX 4060 Laptop；原生Linux 6.8.0-149-generic。
设备：S4Y4NX0R755035，PCI 0000:07:00.0，同一 Samsung 512 GB NVMe。
io_uring 内核 NVMe 与 SPDK VFIO 顺序占用，提交 CPU=4、I/O owner CPU=2。完整环境、实际 SPDK JSON、所有命令和源码/可执行文件 SHA 均保留。
Raw 区域 1 GiB 起、128 MiB；迁移区域为 129 MiB，额外 1 MiB 先单独持久备份。所有区域恢复 SHA-256 一致，驱动、服务、大页和 NVMe poll_queues 原值恢复。
Raw 为相同的非持久完成语义；O_DIRECT、固定文件、注册缓冲。Raw CRC/严格字节验证单独计时，迁移路径 CRC 不关闭。每配置 3 轮预热、30 轮正式，两个交替后端顺序批次，全部原始样本保留。
每轮操作数为 max(QD, min(512, 16 MiB / block_size)) 并按 QD 补齐；128 MiB 小区域及短窗口不能代表全盘稳态写入。

## A：Raw 后端

完整 7 大小 × 7 QD × 5 负载 × 2 后端 × 2 顺序批次，共 980 进程配置、29,400 正式轮次。
下表为每配置 60 轮中位数，尾延迟来自其全部 I/O 实际完成样本。没有选最好一轮。

| 大小/负载 | QD | 后端 | IOPS | MiB/s | P99 µs | cycles/I/O | CPU % |
|---|---:|---|---:|---:|---:|---:|---:|
| 4 KiB rand-read | 1 | uring | 15888 | 62.1 | 71.2 | 40300 | 13.9 |
| 4 KiB rand-read | 1 | spdk | 17493 | 68.3 | 59.4 | 273939 | 105.6 |
| 4 KiB rand-read | 32 | uring | 115873 | 452.6 | 295.3 | 12157 | 37.9 |
| 4 KiB rand-read | 32 | spdk | 122058 | 476.8 | 267.2 | 43202 | 114.3 |
| 4 KiB rand-read | 128 | uring | 176079 | 687.8 | 703.1 | 10588 | 51.3 |
| 4 KiB rand-read | 128 | spdk | 173077 | 676.1 | 698.8 | 30587 | 115.3 |
| 512 KiB seq-read | 1 | uring | 2635 | 1317.5 | 552.0 | 296468 | 26.7 |
| 512 KiB seq-read | 1 | spdk | 2460 | 1230.0 | 686.0 | 1938381 | 114.7 |
| 512 KiB seq-read | 32 | uring | 3882 | 1941.0 | 8727.5 | 249040 | 33.6 |
| 512 KiB seq-read | 32 | spdk | 4216 | 2108.2 | 7614.0 | 1117735 | 103.4 |
| 512 KiB seq-read | 128 | uring | 4099 | 2049.3 | 33793.0 | 259212 | 34.5 |
| 512 KiB seq-read | 128 | spdk | 4451 | 2225.7 | 28618.5 | 1049416 | 102.8 |
| 512 KiB seq-write | 1 | uring | 2754 | 1377.0 | 2887.9 | 228434 | 22.3 |
| 512 KiB seq-write | 1 | spdk | 2890 | 1445.0 | 2889.6 | 1609693 | 106.0 |
| 512 KiB seq-write | 32 | uring | 2451 | 1225.7 | 15598.1 | 241667 | 22.3 |
| 512 KiB seq-write | 32 | spdk | 2468 | 1233.8 | 22602.9 | 1860478 | 104.0 |
| 512 KiB seq-write | 128 | uring | 2545 | 1272.4 | 52842.0 | 248568 | 21.9 |
| 512 KiB seq-write | 128 | spdk | 2519 | 1259.6 | 53270.1 | 1798456 | 101.8 |

| 后端 | 最大真实 bdev/SQE 在途数 |
|---|---:|
| uring | 128 |
| spdk | 128 |

Queue Wait 为提交排队到 dispatch，copy 单独记录。CPU 为计时 I/O 窗口内进程 CPU；PMU cycles 覆盖该进程已存在的线程，包含短计数器控制边界，不是 TSC 估算，不涵盖外部 IRQ/kworker 或 SQPOLL 内核线程的全部消耗。
配对报告保留每轮及每进程批次比值；同进程内轮次相关，只有两个独立顺序批次，不给显著性或生产容量承诺。

## A 结果解读

* 4 KiB 随机读从 QD1 到 QD128：SPDK 约 17.5k→173.1k IOPS，io_uring 约 15.9k→176.1k IOPS，均有清晰的并发扩展。峰值接近，SPDK 并非在所有 QD 都更快。
* 512 KiB 顺序读 QD32：SPDK 比 io_uring 的中位吞吐高 8.6%，P99 约低 12.8%；但 cycles/I/O 约为 4.5 倍，进程 CPU 约 103% 对 34%。这里 100% 相当于占满一个逻辑核，包含提交线程。
* 512 KiB 顺序写 QD32：中位吞吐仅高约 0.7%，SPDK P99 为 22.60 ms、io_uring 为 15.60 ms，SPDK 尾延迟更差。QD128 写入吞吐也没有 SPDK 优势。
* 本实现的零周期 SPDK poller 在空闲时继续占用 owner CPU；进程 CPU/PMU 窗口只统计 I/O 阶段，未把阶段外的轮询消耗算入表中。可选后续研究是有界自适应轮询/中断；本次没有据此预设改进幅度。

这些结果支持“多请求引擎已消除 QD1 串行限制”，不支持“SPDK 在所有负载都比 io_uring 更高效”。

## B：KV Page 迁移

全部为 512 KiB Page，批量 1/32/64/128/256，运行上限 1/4/8/16/32/64/128，CPU/GPU Spill 与 Recovery。280 个进程配置。以下展示 128 Page 的 GPU 迁移；完整 280 个方向分组和累计阶段计时见汇总。

| 方向 | 并发上限 | 后端 | GiB/s 中位数 | P99 batch ms | 实测最大运行数 |
|---|---:|---|---:|---:|---:|
| spill | 1 | uring | 1.036 | 78.871 | 1 |
| spill | 1 | spdk | 1.109 | 59.959 | 1 |
| spill | 16 | uring | 1.230 | 65.879 | 16 |
| spill | 16 | spdk | 1.274 | 60.067 | 16 |
| spill | 128 | uring | 0.957 | 72.034 | 128 |
| spill | 128 | spdk | 1.006 | 69.224 | 128 |
| recovery | 1 | uring | 0.931 | 81.930 | 1 |
| recovery | 1 | spdk | 1.004 | 70.247 | 1 |
| recovery | 16 | uring | 1.627 | 45.356 | 16 |
| recovery | 16 | spdk | 1.807 | 37.396 | 16 |
| recovery | 128 | uring | 1.715 | 38.754 | 128 |
| recovery | 128 | spdk | 1.898 | 36.381 | 128 |

阶段数据分别列出 SSD 等待、CRC、Host memcpy、D2H、H2D；并发阶段累计 wall time 会重叠，不能简单相加还原 elapsed。没有采集精确 GPU/NVMe 时间线重叠比例，也不把 batch P99 冒充单 Page P99。

### 迁移阶段与并发取舍

128 Page（64 MiB）GPU Recovery 的 QD16 中位总时长：io_uring 38.408 ms、SPDK 34.592 ms，对应吞吐高约 11.0%。P99 batch 为 45.356 / 37.396 ms。该配置的累计阶段中位数如下：

| 后端 | SSD 调用/等待 ms | CRC ms | Host copy ms | H2D ms |
|---|---:|---:|---:|---:|
| io_uring | 346.565 | 3.954 | 6.268 | 8.642 |
| SPDK | 303.014 | 3.185 | 5.927 | 7.963 |

这是各并发任务累计阶段耗时，大于 batch 总时长是预期行为。`io_stage_ns` 包含请求调度/等待，不是设备单独 service time；D2H/H2D 包含 Host 等待 Event 的时间，不是 CUDA Event 的纯设备计时。当前证据证明独立 Page 并发且跨 Page 的传输可以重叠，未测出精确重叠百分比。

Recovery 从并发1到64有扩展，128相较64已略微回落；Spill 在高并发下也可能退化。128是能力上限验证，不是推荐默认值；生产应按 CPU 预算、尾延迟和模型等待选择 QD。

## C：CUDA 推理

真实运行 Nano-VLLM 的随机权重 dim=32、2 层 CUDA fixture，逐轮 Token IDs 与无 SSD 基线完全一致。它是功能与回源等待证据，不是生产大模型性能结论。

| 缓存情况 | 后端 | TTFT P50 ms | TTFT P99 ms | 同步恢复等待中位 ms |
|---|---|---:|---:|---:|
| cold-A | uring | 1.443 | 1.563 | 0.000 |
| cold-A | spdk | 1.687 | 1.710 | 0.000 |
| ram-hot-A | uring | 0.100 | 0.118 | 0.000 |
| ram-hot-A | spdk | 0.117 | 0.120 | 0.000 |
| cold-B | uring | 9.726 | 12.231 | 0.000 |
| cold-B | spdk | 1.748 | 3.055 | 0.000 |
| ssd-return-A | uring | 0.269 | 2.758 | 0.108 |
| ssd-return-A | spdk | 0.167 | 0.354 | 0.030 |

恢复等待是推理调度同步等待 SSD→KV 恢复，不是 GPU 硬件 idle counter。A/B/C 不混算加速比。

`cold-B` 包含淘汰 A 并首次写入 SSD 的路径，io_uring 在第一次传输时延迟创建 owner 工作线程，SPDK 的 owner 在 open 时已启动。因此 9.726 / 1.748 ms 的差异包含首次初始化，不能归因为 NVMe 读写本身；后续可将文件后端 owner 预热纳入创建阶段。较适合判断回源路径的是 `ssd-return-A`：TTFT 中位为 0.269 / 0.167 ms，恢复等待约 0.108 / 0.030 ms。这些数字仅适用于该很小的 fixture。

推理每组60个测量样本，P99/P99.9为描述性插值，极端尾分位证据有限；没有宣称这些值代表生产模型SLO。

## 回归、限制与复现

Release/CUDA/SPDK 48 项；ASan+UBSan/CUDA/SPDK 48 项（ASAN_OPTIONS=protect_shadow_gap=0）；TSan/SPDK 43 项（setarch -R），均通过。测试只静态注入 SPDK 资源不足/移除事件，未实际热拔盘。
网络 Reactor/Proactor/NtyCo 独立回归通过；网络源码 SHA 保持，三种执行模型及复制入口完整保留。本机无 RDMA 设备，未声称物理 RDMA 往返通过。
生产未启用共享 CUDA/SPDK DMA Pool；大页内存注册与 CUDA 往返原型已通过。多 Reactor、生产模型级并发 Prefetch/TTFT、掉电原子恢复及实际热移除仍未验证。旧 buffered API 的 MIFSSD1 容量兼容路径保留，对齐两后端为 MIFSSD3。

- [Raw CSV/JSON](benchmarks/spdk-async-2026-11-28/final-v4/raw/summary.csv)，[配对及全部分位数](benchmarks/spdk-async-2026-11-28/final-v4/raw/summary.json)。
- [迁移 CSV](benchmarks/spdk-async-2026-11-28/final-v4/migration/summary.csv)，[阶段/配对 JSON](benchmarks/spdk-async-2026-11-28/final-v4/migration/summary.json)。
- [推理 CSV](benchmarks/spdk-async-2026-11-28/final-v4/inference/summary.csv)，[推理 JSON](benchmarks/spdk-async-2026-11-28/final-v4/inference/summary.json)。
- [环境](benchmarks/spdk-async-2026-11-28/final-v4/raw/environment.json)，[源码 SHA-256](benchmarks/spdk-async-2026-11-28/final-v4/source-sha256.json)。
- 每个目录的 run.json 含完整命令、备份 SHA、结束状态、源码及可执行文件 SHA；*-io.csv 保留每个原始 I/O，*-rounds.csv 包含预热与测量。

```bash
sudo python3 scripts/run_spdk_async_suite.py --skip-profiles --out /absolute/new/result
python3 scripts/render_spdk_async_report.py /absolute/new/result
# 此 runner 锁定本机已核验序列号及备份清单；其他机器先建立其专用设备/区域清单。
```

## 可选 IOPOLL / SQPOLL 物理验证

使用最终相同生产引擎，分别对4/512 KiB、QD1/32/128、随机读/70R30W运行3预热+30测量、两个交替顺序批次。
两种profile各48个配置，全部通过；实际io_uring flags分别为 `(IOPOLL=1,SQPOLL=0)` 和 `(0,1)`，最高在途128。
这两个有限覆盖的profile单独保留，不混入主矩阵。IOPOLL显式配置内核NVMe poll queue，结束后恢复0。

| profile / 随机读 | QD | 后端 | IOPS | MiB/s | P99 µs |
|---|---:|---|---:|---:|---:|
| iopoll / 4 KiB | 128 | uring | 161751 | 631.8 | 812.0 |
| iopoll / 4 KiB | 128 | spdk | 156878 | 612.8 | 774.7 |
| iopoll / 512 KiB | 32 | uring | 3239 | 1619.6 | 10518.6 |
| iopoll / 512 KiB | 32 | spdk | 4107 | 2053.4 | 8526.5 |
| sqpoll / 4 KiB | 128 | uring | 149236 | 583.0 | 2718.6 |
| sqpoll / 4 KiB | 128 | spdk | 153877 | 601.1 | 773.7 |
| sqpoll / 512 KiB | 32 | uring | 3095 | 1547.5 | 11186.6 |
| sqpoll / 512 KiB | 32 | spdk | 4170 | 2085.2 | 8533.1 |

SQPOLL的内核线程与owner设在同一CPU以保持显式核预算，会发生调度竞争；本profile并不声称给出最优多核SQPOLL调优。
主矩阵使用中断完成的io_uring；IOPOLL/SQPOLL并非静默关闭，也没有只选较慢结果替代主矩阵。
profile在主矩阵之后另测，频率/温度/设备状态可能不同，只与各自profile里的配对SPDK比较，不跨批次拼接峰值。

* [IOPOLL完整汇总](benchmarks/spdk-async-2026-11-28/final-v4/iopoll/summary.json)
* [SQPOLL完整汇总](benchmarks/spdk-async-2026-11-28/final-v4/sqpoll/summary.json)

```bash
sudo python3 scripts/run_spdk_async_ab.py --smoke --iopoll --out /absolute/new/iopoll
sudo python3 scripts/run_spdk_async_ab.py --smoke --sqpoll --out /absolute/new/sqpoll
```

## CUDA / SSD 跨 Page 流水线时间线

额外使用Nsight Systems采集128×512 KiB Page、迁移并发16的3预热+3分析轮次。分析程序链接原迁移benchmark对象和生产库，仅以link wrapper添加NVTX区间，不替换I/O算法或Buffer路径；生产二进制没有重编译。

| 后端 | D2H与其他Page SSD写调用重叠的CUDA执行时间占比 | H2D与其他Page SSD读调用重叠的CUDA执行时间占比 |
|---|---:|---:|
| io_uring | 93.92% | 93.97% |
| SPDK | 94.59% | 93.74% |

每种方向各有768次512 KiB CUDA设备拷贝。占比的分母是CUDA设备拷贝累计执行时间；分子是其与**另一线程、另一Page的未完成SSD调用**相交部分，重叠区间先求并集避免重复累计。
SSD调用包含队列、Host copy和完成等待，因此该证据证明跨Page的异步调度重叠，**不能解释为NVMe设备DMA与GPU copy engine的纯硬件重叠率**，也不替代未插桩吞吐统计。

* [时间线图](benchmarks/spdk-async-2026-11-28/final-v4/pipeline-trace4/pipeline-overlap.png)
* [精确统计口径与结果](benchmarks/spdk-async-2026-11-28/final-v4/pipeline-trace4/overlap.json)
* `pipeline-trace4/`同时保留原始`.nsys-rep`、SQLite、逐事件区间CSV、生产库SHA及恢复检查。

```bash
sudo python3 scripts/profile_spdk_async.py --out /absolute/new/trace
python3 scripts/summarize_spdk_pipeline_trace.py /absolute/new/trace
```

前几次分析器采集因编译环境宏/目标进程降权失败，诊断日志保留在pipeline-trace、pipeline-trace2、pipeline-trace3；未使用其成绩。
成功的采集显式保留已授权的root分析器上下文，设备节点权限未放宽；结束后原始区域SHA及VFIO服务状态均恢复。

最终完整文件清单、环境与校验：

* [41个任务文件及SHA](benchmarks/spdk-async-2026-11-28/final-v4/task-files.json)
* [全部硬件恢复与测试验收](benchmarks/spdk-async-2026-11-28/final-v4/final-validation.json)
* [工具链和CPU拓扑](benchmarks/spdk-async-2026-11-28/final-v4/toolchain.json)

尚未消除的工程开销包括SPDK空闲轮询、io_uring首次传输的延迟线程启动、迁移高并发时的Host调度及拷贝成本。
阶段B的Recovery batch覆盖SSD→GPU并行加载；没有额外声称已完成生产推理服务的多请求预取调度或模型SLO验证。

[统一实验索引和未验证范围](benchmarks/spdk-async-2026-11-28/final-v4/result.json)。
