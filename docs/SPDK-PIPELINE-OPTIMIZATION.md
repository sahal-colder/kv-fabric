# SPDK 回载流水线优化实测

2026-11-28，Australia/Perth，正式运行 11:49:03–11:51:23。基准源码提交 `0ad62c3`。

**已实际降低 SPDK 回载延迟约 25%，但主要收益来自异步流水线，而非换后端本身。** 同三星原盘、16 MiB KV、QD=4、默认 powersave 下，SPDK GPU 可用延迟 P50 **9.789855 → 7.345553 ms（−24.97%）**，P95 **10.287459 → 7.615879 ms（−25.97%）**。四批中 P50 降低 24.03%–26.18%，方向一致。io_uring 接入同样流水线后为 7.390797 ms，SPDK 的额外 P50 优势仅 0.61%。

本轮交付的是**后端回载基准的实际代码与实测**。生产 `mi_storage_migrate`、Nano-VLLM 推理和 SSD 前缀缓存的调用路径没有接入这套批量流水线；本轮没有重测推理 TTFT，不能把 25% 宣称为生产推理改善。此前 io_uring 裸设备推理后端的缺口仍在。

## 改了什么

原路径每轮先读完全部 32 块，再校验全部数据，再向 GPU 搬运全部数据：

```text
全部 SSD 读取完成 → 全部 CRC 完成 → 全部 H2D 完成 → GPU 同步
```

新路径在某块读完后立刻交给主线程，校验通过后提交该块的异步 H2D；I/O 线程继续读后续块。每块使用独立且不复用的缓冲区，到全部 H2D 同步完成后才开始下一轮：

```text
SSD 读取下一块
    与 已完成块的 CRC、H2D 重叠
    最后等待全部 GPU 搬运完成
```

实现见 [kvf_backend_compare.c](../tools/kvf_backend_compare.c)：

- SPDK 继续使用 vfio-pci + PCIe NVMe bdev，直接绕过内核块层；每次 bdev 完成回调发布对应记录的就绪状态，继续补交请求。
- io_uring 使用单独的 CPU 2 工作线程消费 CQE、补交读取，主线程仍为 CPU 4；采用相同的就绪队列、顺序消费、CRC、CUDA stream 和缓冲区生命周期。
- 就绪状态由 mutex/condition variable 同步。最后一个 I/O 完成及 GPU 同步均须确认，不通过假完成缩短计时。
- 保留每轮全部身份、格式版本、数据版本和 CRC 检查；计时后仍逐字节核对全部 CPU 数据及 GPU 回读数据。
- 扩展真实逻辑 QD 到 4/8/16/32，保留原有 QD1 支持。每轮均检查请求峰值等于配置、提交/完成各 32 次。

异步提交加锁页主机内存允许主机计算与 GPU 传输重叠，参考 [CUDA 官方说明](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html#asynchronous-and-overlapping-transfers-with-computation)。SPDK 的异步队列及单线程拥有队列的规则见 [SPDK 官方说明](https://spdk.io/doc/nvme.html)。本轮没有使用 GDS，数据仍经过锁页主机内存。

## 同轮、同负载的正式对照

硬件仍为三星 MZVLQ512HALU-00000、序列号 `S4Y4NX0R755035`、PCI `0000:07:00.0`、RTX 4060 Laptop、i7-13700H。块大小、物理偏移、固定数据记录及 CRC 实现均沿用[前次对照](SPDK-IOURING-COMPARISON.md)。每轮有效载荷 32 × 512 KiB = 16 MiB；实际读盘 16.125 MiB。原测试区仍为从 1 GiB 开始的 128 MiB。

两后端都重测，每批交替后端顺序；同时测原 powersave 及 CPU 2/4 的 performance governor，奇偶批次反转 governor、后端和配置顺序。**performance 是 governor 策略，不等于固定频率**，未改变其他 CPU 策略、Turbo 或 GPU 频率。

四批 × 两种 governor × 两后端 × 五种配置（串行 QD4、流水线 QD4/8/16/32），共 80 个进程。每进程预热 3 轮、正式 30 轮：每组 120 正式样本，共 **2400 正式、240 预热**。P50 为中位数，P95 为 nearest-rank，所有原始样本保留。

**主指标仍从首批读取发出前开始，到最后一次 GPU stream 同步完成为止。** 初始化、温度读取和事后逐字节比对不计入。没有用历史 18.95 ms 作本次优化前基线；优化前基线在本轮同批重新测量。

默认 powersave、相同实际逻辑 QD=4 的结果：

| 后端 / 策略 | GPU 可用 P50，ms | P95，ms | 有效 GiB/s @P50 | 每轮窗口内进程 CPU 时间 P50，ms |
|---|---:|---:|---:|---:|
| io_uring，分阶段串行 | 9.737572 | 10.627177 | 1.604609 | 2.816326 |
| io_uring，流水线 | 7.390797 | 7.740378 | 2.114116 | 1.817493 |
| SPDK，分阶段串行 | **9.789855** | **10.287459** | 1.596040 | **12.510462** |
| SPDK，流水线 | **7.345553** | **7.615879** | **2.127137** | **8.379677** |

SPDK 的流水线优化使 P50 减少 2.444302 ms（24.97%）、P95 减少 2.671580 ms（25.97%），有效吞吐提高约 33.28%。窗口内进程 CPU 时间 P50 减少 33.02%，包含轮询线程。io_uring 的同类优化使 P50 减少 24.10%、P95 减少 27.16%，证明大部分收益可由共同的流水线解释。

| 批次 | SPDK 流水线相较本批串行：P50 改善 | P95 改善 | 两边均流水线：SPDK 相较 io_uring 的 P50 改善 |
|---|---:|---:|---:|
| 1 | 25.52% | 27.63% | 0.82% |
| 2 | 24.09% | 27.14% | 0.35% |
| 3 | 24.03% | 24.51% | 0.81% |
| 4 | 26.18% | 23.11% | 0.34% |

流水线收益在四批中可重复。匹配流水线后，SPDK P50 比 io_uring 小约 45 微秒（0.61%），但 CPU 时间仍为其 **4.61 倍**。对应 P95 的合并改善为 1.61%，逐批范围 **−1.30%–+18.73%**，方向并不始终一致，不宜表述为稳定的尾延迟优势。

## 加深 QD、切 governor 是否有效

| 实际逻辑 QD | powersave：io_uring 流水线 P50，ms | powersave：SPDK 流水线 P50，ms | performance：io_uring 流水线 P50，ms | performance：SPDK 流水线 P50，ms |
|---|---:|---:|---:|---:|
| 4 | 7.390797 | 7.345553 | 7.397326 | 7.341954 |
| 8 | 7.394984 | 7.356830 | 7.395885 | 7.342887 |
| 16 | 7.397063 | 7.351437 | 7.389708 | 7.354375 |
| 32 | 7.412401 | 7.362218 | 7.408983 | 7.361284 |

加深 QD 没有带来进一步的总延迟改善。SPDK 最低 P50 是 performance + 流水线 QD4 的 7.341954 ms，与 powersave 同配置只差约 3.6 微秒（0.05%）；逐批差异约 −0.03%–+0.10%，不支持“切 governor 大幅提速”的说法。

按各后端最优配置比较，performance 下 io_uring QD16 为 7.389708 ms，SPDK QD4 为 7.341954 ms，SPDK P50 改善 0.65%，窗口内 CPU 时间约 4.62 倍。该比较与同 QD4 的对照分别保留，不拿不同 QD 制造后端收益。

本轮 performance QD4 的 SPDK / io_uring 合并 P95 为 7.434559 / 8.182264 ms（改善 9.14%），但该改善逐批约 0.68%–21.26%，尾部波动较大；原始样本全部保留。主结论采用开测前的 powersave 配置，不通过挑选 governor 或尾部样本夸大收益。

## 为什么流水线有效，以及仍有哪些限制

powersave 下，SPDK 流水线的读取完成 P50 为 **7.263040 ms**，GPU 可用为 **7.345553 ms**。校验与 H2D 仍然执行，但大部分工作被隐藏在持续读盘期间，最后仅留下约一个记录的校验/搬运尾部。这与原来把约 7.3 ms 读取、约 1 ms 校验、约 1.4 ms H2D 串行相加的结构不同。

QD4 到 QD32 的观测都停留在约 7.3 ms，显示当前配置的进一步瓶颈在读取路径附近。**尚未测得控制器理论极限，也没有证明这就是 SSD 的绝对上限。** 在当前实验中，继续增加 QD 或换 governor 没有测出有用收益，不能承诺只靠 SPDK 把 16 MiB 降到 1–2 ms。

流水线 CSV 的阶段字段会重叠，解读时须注意：

- `read_ms`：首批读取到最后一次存储完成回调的墙钟时间，同时包含与主线程处理的重叠。
- `crc_ms`：逐块校验工作区间之和；它仍执行并消耗 CPU，不能因总延迟下降而认为校验被跳过。
- `h2d_ms`：流水线模式下是主线程提交 H2D 的耗时加最后同步等待，**不是完整 GPU DMA 时间**。不能用它与串行 H2D 时间相除宣称 PCIe 搬运变快。
- `read_cpu_ms`：为兼容旧 CSV 留下的字段；流水线模式下统计到全部 CRC/H2D 提交结束的进程 CPU 时间，不能当成纯读取 CPU。
- `total_ms`、`cpu_ms`：两种策略的主端点与进程 CPU 时间可直接比较。汇总脚本不会把重叠阶段相加。

进程 CPU 指标还有两个口径。`cpu_ms` 是主计时窗口内所有线程 CPU 时间；runner 另用 `RUSAGE_CHILDREN` 保存每个完整进程的 user/system CPU 时间，包括初始化、预热和事后校验。powersave QD4 每进程 3+30 轮、四进程的完整生命周期 CPU 时间均值为：

| 后端 | 串行，CPU 秒/进程 | 流水线，CPU 秒/进程 |
|---|---:|---:|
| io_uring | 0.801877 | 0.434710 |
| SPDK | 2.063053 | 2.039492 |

因此，SPDK **窗口内 CPU 时间降低 33%不等于整个进程 CPU 时间降低 33%**。初始化和计时外仍轮询等成本尚在；本轮 SPDK 完整进程 CPU 只减少约 1.14%。对于长期驻留服务，需要单独测稳态请求及空闲轮询成本，不能直接照搬启动一次的进程成本。

## 数据校验和状态恢复

80 个进程、2640 个含预热轮次均通过：实际逻辑 QD、32 次读取全部完成、每记录 ID/格式/版本/CRC、全部主机字节和 GPU 回读字节的检查。没有删样、截尾或复用计时前已在 GPU 的数据；每轮读取前会污染主机缓冲区，每轮重新完成真实 SSD 读取和 H2D。

SSD 整数温度样本范围 29–61°C。两个 governor 的四批顺序交替；未锁定实际频率，未采集热节流计数，不宣称排除了全部温度和调度影响。

开测前再次按序列号确认三星盘，核对既有备份及首/测试区/尾三处 O_DIRECT SHA-256；固定记录只准备一次，80 个正式进程后恢复完整 128 MiB。恢复后 fsync、flush block buffers、O_DIRECT 独立回读三处，均与原值匹配：

| 区域 | SHA-256 |
|---|---|
| 首 1 MiB | `5f80513ceef293b98d109e6a4746ef36ec28cf503159bf746dc28433999e9a15` |
| 1 GiB 起的 128 MiB 测试区 | `254bcc3fc4f27172636df4bf32de9f107f620d559b20d760197e452b97453917` |
| 尾 1 MiB | `30e14955ebf1352266dc2ff8067e68104607e750abb9d3b36582b8af909fcb58` |

设备恢复 vfio-pci，服务恢复 active/exited、enabled，CPU 2/4 governor 均恢复 powersave。未格式化、分区或对 UMIS 系统盘作裸设备写入。原备份继续保留在本地忽略目录，未加入仓库。

## 复现和交付

在原验收机、原备份和已部署服务存在的前提下执行；新输出目录必须不存在：

```bash
cmake -S tools/backend_compare -B build-backend-compare \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=89 \
  -DNANO_WITH_CUDA=ON -DMINIINFER_BUILD_VLLM_BRIDGE=ON \
  -DMINIINFER_VLLM_CUDA=ON
cmake --build build-backend-compare --target kvf_backend_compare -j6
sudo python3 scripts/run_spdk_pipeline_tune.py \
  --out docs/benchmarks/spdk-pipeline-new-run --batches 4
python3 scripts/summarize_spdk_pipeline_tune.py \
  docs/benchmarks/spdk-pipeline-new-run

# 只读重算本次样本，不操作设备：
python3 scripts/summarize_spdk_pipeline_tune.py \
  docs/benchmarks/spdk-pipeline-2026-11-28
```

[数据目录](benchmarks/spdk-pipeline-2026-11-28/)包含：80 份原始 CSV、80 份进程日志、`run.json` 中的实际命令及完整进程 CPU、`summary.json`、极端样本清单、恢复证据、环境、源码 SHA-256、实际工作树源码归档和独立重新构建日志。工作区原本已有未提交依赖，因此仍以归档和哈希补足提交号；历史数据没有覆盖。

简历可写：

> 在原生 Ubuntu、三星 NVMe 与 RTX 4060 Laptop 上实现并验证 KV 读取、CRC 与 H2D 异步流水线，四批同 QD=4 实测将 16 MiB KV 的 SPDK GPU 回载 P50/P95 从 9.79/10.29 ms 降至 7.35/7.62 ms，延迟降低约 25%，并完成原盘恢复和 SHA-256 核验。

面试时需说明：这是相较本轮重测的分阶段串行 SPDK 路径；同样优化后的 io_uring P50 为 7.39 ms，SPDK 额外优势约 0.61%，CPU 成本仍高；生产推理 TTFT 尚未验证。
