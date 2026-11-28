# Native Linux final benchmark archive

本轮 ID：`20261125T153218+0800`。全部简历性能数字仅来自本轮原生 Linux 归档。任务开始前文件位于 `prior-unverified-*`，不参与统计。

## 环境与口径

- Ubuntu 22.04.5，Linux 6.8.0-138-generic；i7-13700H，约 16 GiB 内存。
- RTX 4060 Laptop GPU，NVML 总显存 8188 MiB；驱动 615.71.09。CUDA 编译器 13.4.92；最终推理环境 PyTorch 2.13.0+cu130、vLLM 0.31.0。
- 物理 UMIS NVMe，ext4 `/dev/nvme0n1p6`；Direct I/O 文件目录见 `env.txt`、`planned-matrix.json` 和每组 environment.json。
- Qwen2.5-3B-Instruct，434 个 BF16 张量，权重分片 SHA256 与下载元数据匹配，详见 `model-manifest.json`。
- 固定合成 Token ID，4K/8K/16K 输入，50% 共享前缀，输出 16 Token。GPU KV 1 GiB，外部缓存有效载荷预算 1.5 GiB。
- 全组统一 eager、VLLM_BATCH_INVARIANT=1；每格完整长度预热 3 次、实测 15 请求。每模式/并发为独立服务进程，同一服务顺序扫长度；共 27 格、405 请求。
- 默认 BF16 冷热数值差异在纯 vLLM GPU APC 也复现；见 `numerical-diagnostic/`。最终严格 Token ID 断言未放宽。
- 32K 输入加输出超过模型原生 32768 上限；8K 并发 4/8 超出本轮 1 GiB KV 预算，裁剪为 1/2；LMCache 初始环境未安装，按可选条件不纳入；均无虚构数据。

## 16K 单并发关键总表

除重算外，下表均实测 50% 外部命中；Native 为逐请求清空 GPU APC、保留 Host 的对照。单位：TTFT 为秒，TPOT 为毫秒。

| 路径 | TTFT P50 | P95 | P99 | TPOT P50 | req/s | 输出 Token/s |
|---|---:|---:|---:|---:|---:|---:|
| vLLM 原生重算 | 5.647 | 5.651 | 5.651 | 56.66 | 0.1540 | 2.46 |
| Native Offload (Host 50%) | 3.248 | 3.258 | 3.258 | 56.59 | 0.2442 | 3.91 |
| KV-Fabric CPU | 3.287 | 3.301 | 3.301 | 56.52 | 0.2419 | 3.87 |
| KV-Fabric NVMe fixed | 5.006 | 5.255 | 5.255 | 56.30 | 0.1707 | 2.73 |
| KV-Fabric Direct POSIX | 5.034 | 5.254 | 5.254 | 56.50 | 0.1694 | 2.71 |
| 旧 Connector (逐段/同步写回) | 25.657 | 27.864 | 27.864 | 56.84 | 0.0386 | 0.62 |

n=15 的 nearest-rank P95/P99 均为样本最大值。所有 4K/8K/16K、TPOT/ITL 分位、吞吐及真实命中率见 `matrix-summary.csv` / `matrix-summary.json`。

## 8K 多并发与真实命中层

| 矩阵/模式 | 并发 | TTFT P50 ms | P95 ms | 输出 Token/s | GPU / CPU / NVMe 命中率 |
|---|---:|---:|---:|---:|---|
| matrix-c1 / kvfabric-cpu | 1 | 1344.58 | 1350.34 | 7.73 | 0.00% / 50.00% / — |
| matrix-c1 / kvfabric-disk | 1 | 2414.08 | 2604.00 | 5.19 | 0.00% / — / 50.00% |
| matrix-c1 / native | 1 | 1310.07 | 1318.52 | 7.85 | 46.67% / 3.33% / — |
| matrix-c1 / recompute | 1 | 2410.88 | 2415.75 | 5.10 | 0.00% / — / — |
| matrix-c2 / kvfabric-cpu | 2 | 1652.05 | 2850.91 | 8.83 | 0.00% / 50.00% / — |
| matrix-c2 / kvfabric-disk | 2 | 2446.15 | 3655.52 | 7.36 | 0.00% / — / 50.00% |
| matrix-c2 / native | 2 | 1447.29 | 2669.55 | 9.31 | 46.67% / 3.33% / — |
| matrix-c2 / recompute | 2 | 2576.59 | 5194.44 | 5.68 | 0.00% / — / — |
| matrix-native-external-c1 / native | 1 | 1315.62 | 1318.80 | 7.83 | 0.00% / 50.00% / — |
| matrix-native-external-c2 / native | 2 | 1317.06 | 2679.35 | 9.28 | 23.33% / 26.67% / — |
| matrix-posix-c1 / kvfabric-disk | 1 | 2426.36 | 2514.18 | 5.14 | 0.00% / — / 50.00% |
| matrix-posix-c2 / kvfabric-disk | 2 | 3364.25 | 4655.61 | 7.09 | 0.00% / — / 50.00% |

普通 Native Offload C1 实际为 GPU 46.67% + Host 3.33%。逐批清 GPU 的 C2 仍可在同一 burst 内共享 GPU 前缀，实测 GPU 23.33% + Host 26.67%，不能称作纯 50% Host 回源。Native external 组吞吐为请求 burst 耗时之和，排除 reset RPC；其余为连续请求窗口。— 表示不适用/没有独立计数，不填作零。

## Nsight Systems

两版各 2 个 16K/50%/并发1/输出16请求；先执行 5 次完整长度预热使外部缓存饱和。capture-range=cudaProfilerApi，capture-range-end=stop，cuda-flush-interval=0。每请求 288 MiB H2D 完整覆盖硬校验均通过。

| 指标 | 旧路径 | 优化版 |
|---|---:|---:|
| H2D 载荷拷贝/请求 | 18432.0000 | 4.0000 |
| H2D DMA GiB/s | 7.1068 | 10.0282 |
| D2H DMA GiB/s | 8.1505 | 9.8440 |
| H2D Host 提交+同步 ms | 432.2263 | 30.5602 |
| 其中 H2D 纯 Host 同步等待 ms | 1.1265 | 30.2602 |
| 前台写回等待/发布 ms（扣除模型计算） | 18945.9489 | 0.0000 |
| 全部前台写回非计算开销 ms | 19320.6885 | 0.0000 |
| H2D Compute overlap % | 0.0000 | 0.0000 |
| D2H Compute overlap % | 0.0000 | 99.3939 |

带宽按完整相关 DMA 活动计算，避免 TTFT 窗口裁尾高估。Overlap 为 TTFT 窗口内 DMA 活跃时间与模型 GPU Kernel 的交集比例；按 correlation ID 排除 Gather/Scatter 及旧路径通用拷贝 Kernel。纯 H2D 同步等待增大，是批量异步提交后将 DMA 等待集中到同步点；H2D 提交+同步总时间显著下降。

## CUDA 与 Nsight Compute

| CUDA 模式 | 方向 | payload memcpy | P50 ms | 有效 GiB/s | 样本 |
|---|---|---:|---:|---:|---:|
| dma | H2D | 1 | 6.089984 | 11.5456 | 15 |
| dma | D2H | 1 | 6.067104 | 11.5891 | 15 |
| segments | H2D | 4608 | 13.674208 | 5.1420 | 15 |
| segments | D2H | 4608 | 11.882240 | 5.9174 | 15 |
| vector | H2D | 1 | 6.838272 | 10.2822 | 15 |
| vector | D2H | 1 | 7.037216 | 9.9915 | 15 |

72 MiB，各方向预热3次、实测15次，共90条原始计时；每模式/方向结束后逐字节校验。元数据和验证搬运不计入 payload memcpy 次数。

| Kernel | DRAM 利用率 | DRAM GiB/s | Achieved Occupancy | 读/写 sector 利用率 |
|---|---:|---:|---:|---|
| gather_kv_pages | 90.2585% | 214.9946 | 91.3220% | 93.3929% / 100.0000% |
| scatter_kv_pages | 90.0384% | 187.6602 | 89.9133% | 93.3929% / 100.0000% |

每 Kernel 1 个 launch、37 replay passes。原始 DRAM Gbyte/s 已除以 2^30 换算为 GiB/s。初始 ERR_NVGPUCTRPERM 已经 sudo 采样解决，原始失败及成功报告均保留；未修改驱动权限配置。

## 物理 NVMe 三级回源

| 路径 | P50 ms | P95/P99 ms | 有效 GiB/s | 实测次数 |
|---|---:|---:|---:|---:|
| direct-uring-fixed | 58.826840 | 60.467439 | 1.195245 | 15 |
| direct-posix | 59.788541 | 61.538177 | 1.176020 | 15 |

72 MiB、144×512 KiB 块，GPU→Host→SSD 显式迁移后确认 SSD 驻留，再计时 SSD→GPU，包含 CRC、Staging、H2D 与设备同步。每次计时后逐字节校验。该几何与 LLM 的逐层16 KiB记录不同，不代表裸盘带宽；O_DIRECT 绕过 OS 页缓存，SSD 控制器缓存未清空。

## 可靠性与解释边界

- 两套 Release+CUDA 构建均 17/17 CTest 通过，无跳过。完整 Connector 冷算、CPU/NVMe 热命中、部分命中、超时回退通过，独立 GPU 层块检查 3,384 次。
- 405条原始输出独立复核通过，345条非参考请求逐 Token ID 匹配；另有15条跨并发重算参考一致性检查。
- 16K CPU计时窗口发生42次 save_skipped，NVMe fixed发生39次；加载失败与写入错误均为0。save_skipped 是槽忙时最佳努力缓存写入策略，不能等同于所有KV完整持久写回；各格事件数在 matrix-summary.csv 和独立自查表。
- 旧路径整体 TTFT 与优化版差异包含同步/后台发布、本地归属页读取快路径、有界跳过写入等策略，不全归因于单个向量化 Kernel。
- 上游 vLLM 停服存在 resource_tracker 单个 semaphore 清理 warning，日志保留；未执行 sanitizer/RDMA 实机性能或资源泄漏专项，不声明这些结论。
- 初次全会话 nsys 第二请求缺 CUDA activity，保留于 profiling-full-capture-incomplete，未用于简历；最终区间采集四条请求均完整通过288MiB硬校验。
- 默认数值差异、预热等待口径和组间写回队列诊断尝试分别保留，最终成功执行状态见 execution-status.json。

## 交付与复现

- 简历所有指标/公式/来源：resume-evidence.json；原始输出独立校验：raw-token-audit.json。
- 环境：env.txt；模型：model-manifest.json；完整编译、CTest XML、原始 CSV/JSONL、.nsys-rep/.sqlite/.ncu-repz 均位于本目录。
- 复现实验：使用 run.json 中的私有 venv/bin/python 执行 scripts/run_native_linux_final.py。所有GPU/loopback/io_uring操作需在原生设备可见的执行环境中运行。
- 文档：python-docx 创建、捆绑 LibreOffice 导出、标准 render_docx.py 工作流渲染核验。Ubuntu22兼容库仅提取于私有工作目录，未安装/升级系统包。
- 终版 PDF 与 DOCX 渲染 PNG 逐像素一致，严格1页、全部文字匹配Markdown，面试自查表未混入Word/PDF。
- A4；Noto Sans CJK SC 正文 10pt、固定 12.0pt 行距；姓名22pt、栏目12pt、联系信息9.5pt；#1F4E79、0.75pt底线；上下1.3cm、左右1.45cm；纵向占用88.88%。
