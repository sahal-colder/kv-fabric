# README 视觉补强与导出清单

建议按“架构 → 实际时间线 → 带宽口径”放三张图。源数据和图片均在 `docs/benchmarks/perf-attribution-2026-11-26/`；可直接嵌入 PNG，SVG 用于放大审阅。

| 图 | 要表达的事实 | 已生成图片与可审计来源 |
|---|---|---|
| 系统架构与证据边界 | 目录控制面、GPU/CPU/NVMe 分层、pinned staging、TCP/rsockets；RDMA 单独列为跨 Worker 数据面 | `figures/architecture.{png,svg}`；源码 `scripts/render_perf_attribution.py` |
| nsys 回源并发时间线 | 同一输入的逐层 QD1 与四 lane owned read；H2D、模型计算、后台 D2H 的真实区间；不画不存在的 H2D/Prefill overlap | `figures/nsys-read-concurrency.{png,svg}`；`nsys-before-diagnostic/trace.{nsys-rep,sqlite}`、`nsys-final-disk/trace.{nsys-rep,sqlite}`、`attribution.json` |
| ncu 带宽口径对照 | 单向有效 payload、物理 DRAM 总流量、实际显存时钟、DRAM utilization、store sector；解释 Scatter 的分母 | `figures/ncu-bandwidth-accounting.{png,svg}`；原始 `native-linux-final/ncu-root/{gather,scatter}.{csv,ncu-repz}` 与本轮 `ncu/` |

```bash
python3 scripts/summarize_perf_attribution.py docs/benchmarks/perf-attribution-2026-11-26
python3 scripts/render_perf_attribution.py docs/benchmarks/perf-attribution-2026-11-26
```

第三张目前是原始计数重绘的对照面板，**不是伪造的 ncu GUI 截图**。如果 README 需要 GUI 截图，用 `ncu-ui` 分别打开原始 Gather/Scatter `.ncu-repz`，并排展示 Speed Of Light、Memory Workload Analysis 和 Raw 页中的 `dram__bytes_read/write.sum`、`dram__cycles_elapsed.avg.per_second`、store sectors；人工导出到 `figures/ncu-gui-memory-workload.png`。该 GUI 截图路径为建议导出路径，目前没有声称文件已生成。

RDMA 性能表建议紧随架构图，链接 `rdma-payload-final/summary.json` 和 MTU 对照；标题明确“单机 RXE，含控制/连接/CRC/提交”，不能标注物理 RoCE 链路带宽。原始大报告留在证据包中，README 只展示图片和精简表格。

## 跨实例 RDMA 新增三图

| 图 | 路径 | 来源 |
|---|---|---|
| 双节点与 pinned ring 架构 | `benchmarks/rdma-cross-instance-2026-11-26/figures/architecture.{png,svg}` | 真实 netns/SIW 进程布局，明确单 GPU 时分预热 |
| RDMA/H2D 重叠 | `benchmarks/rdma-cross-instance-2026-11-26/figures/nsys-rdma-h2d.{png,svg}` | CUDA correlation 关联后的 receive / DMA / Scatter 区间；不是示意重叠 |
| 冷重算 vs 跨实例 TTFT | `benchmarks/rdma-cross-instance-2026-11-26/figures/ttft.{png,svg}` | 同一请求集，保留 Baseline 失败回退样本 |

生成：`python3 scripts/render_rdma_cross_instance.py docs/benchmarks/rdma-cross-instance-2026-11-26`。完整原始 nsys 留在 `.kvfabric-local/rdma-cross-instance/{final-rdma,verified-final-rdma}/profile-*/trace.{nsys-rep,sqlite}`，公开区间与 SHA256 在证据目录中。
