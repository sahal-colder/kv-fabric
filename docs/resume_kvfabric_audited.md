# KV-Fabric 简历项目段落（审计版）

RTX 4060 Laptop 8 GiB、Qwen2.5-3B BF16；下列 TTFT 对应 16K 输入、8K 共享前缀、16 输出 Token。RDMA 为独立的 Storage / Nano-VLLM 数据面验证，不混用 Qwen TTFT。

- **前缀复用与首 Token**：显存受限导致共享前缀重复 Prefill → 通过 vLLM V1 异步匹配、批量导入和后台发布复用 CPU KV → TTFT P50 **5.647 → 3.287 s，下降 41.8%**，输出 Token ID 与冷算一致。
- **CUDA 数据路径**：离散 KV 的逐段提交开销高 → 按物理页与 stride 实现 `uint4` Gather/Scatter、pinned 批量搬运及 Event 交接 → 288 MiB 回传的 H2D payload 拷贝 **18,432 → 4 次**、Host 提交＋同步 **432.2 → 30.6 ms**；72 MiB 核函数单向有效载荷带宽 **119.13 / 103.54 GiB/s**。
- **NVMe 回源**：16 KiB 逐层记录串行读取阻塞首 Token → 以 `O_DIRECT + io_uring` fixed buffer 承载校验后的 owned snapshot，按深度并发读取并为后台写回预留容量 → 同负载 TTFT P50 **4.961 → 3.675 s，下降 25.9%**，load/save error 均为 0；独立文件对照中避免 **160 MiB Page Cache** 驻留。
- **跨 Worker RDMA 数据面**：跨实例 KV 复用需要一致性与失败收敛 → 实现 rsockets、连接池、批量 CRC/租约校验及回退 → 单机 Soft-RoCE（MTU 9000）的 **16 MiB CUDA KV 恢复 P50 30.20 ms、有效 0.517 GiB/s**；小模型前缀复用跳过 **120/124 Token 的 Prefill**，断链/超时后的本地重算与重连恢复均保持 Token 一致。

数字来源与边界见 [技术审计报告](PERFORMANCE-ATTRIBUTION-AUDIT.md)。前两条来自原始 native-linux-final；第三条来自本轮修前/最终修后；第四条 Token 复用来自已归档 stories15M 配对测试，16 MiB 恢复来自本轮 MTU 9000 的 TCP/RXE 配对实测。DRAM utilization、物理 DRAM 带宽与有效载荷带宽不混写；不宣称 Soft-RoCE 胜过 TCP 或物理 HCA 加速。
