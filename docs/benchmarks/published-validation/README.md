# 公开验证数据

这些文件由已有实测记录按字段白名单导出；不含用户联系方式、主机身份、绝对路径、私人备份、模型权重或原始环境日志。源测量没有因脱敏改变数值。历史与当前代码分别标注，不能将历史结果冒称最新代码的新测结果。

| 文件 | 范围 |
|---|---|
| `historical-vllm-matrix.json` | 2026-11-25 原生 Linux、Qwen2.5-3B BF16、vLLM 0.31.0，27格、405请求 |
| `historical-token-measurements.json` | 405条合成输入对应的输出Token与延迟；345条非参考请求严格对照 |
| `historical-nsys-summary.json` | 旧/优化各2个请求，16K、50%外部命中，完整288MiB H2D载荷归因 |
| `historical-ncu-summary.json` | Gather/Scatter硬件计数，不是由标称带宽推算 |
| `historical-cuda-summary.json` | 72MiB传输微基准，各方向预热3次、15样本 |
| `historical-recovery-summary.json` | 物理NVMe文件路径Direct fixed/POSIX三级回源 |
| `historical-rdma-pairs.json` | 较早WSL2同机SoftRoCE，CPU/CUDA成对实测；不是HCA性能 |
| `current-connector.json` | 审计修复后真实模型CPU/NVMe冷算、热命中、部分命中、超时8类用例 |
| `current-rdma.json` | 修复后原生Linux临时RXE，CPU/CUDA各30轮严格RDMA及逐字节检查 |
| `current-nvme.json` | 修复后物理NVMe/ext4，72MiB、144块、15样本、CRC与逐字节核验 |
| `nano-cuda-summary.json` | 独立nano-vLLM-C项目、明确提交与算子/Decode测试范围 |

所有n=15的nearest-rank P95/P99都是样本最大值。eager、batch-invariant、GPU/Host容量、实际命中层与`save_skipped`策略均须一起解释。普通Native Offload可能包含GPU APC，不能一概称为50%纯Host回源。旧Connector到优化版的TTFT差异包括批量搬运、归属页读取快路径、后台发布和槽忙时跳过缓存写入，不只来自一个Kernel。

RDMA功能验证、物理NVMe文件I/O、完整vLLM单机集成均已完成。2026-11-28 补齐 SPDK 裸 NVMe 接管、CPU/GPU 分层回源与小模型推理缓存恢复；[脱敏汇总与 60 个样本](../spdk-nvme-2026-11-28/summary.json)独立记录，不与历史 Qwen TTFT 混用。尚未覆盖物理HCA跨机/GPUDirect、运行中物理NVMe移除、在线GPU故障恢复，以及生产多卡SLA。未知DMA完成时的资源保留单列于验证报告，不冒充零泄漏。

修复后新增12格/180请求结果见`current-vllm-matrix.json`、`current-token-measurements.json`；历史27格405请求另存。`current-vs-historical-ttft.json`保留相同参数跨日期对照，不当作随机配对实验。
