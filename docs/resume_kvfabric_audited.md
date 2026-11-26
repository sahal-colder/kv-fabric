# KV-Fabric 简历项目段落（审计版）

RTX 4060 Laptop 8 GiB、Qwen2.5-3B BF16；下列 TTFT 对应 16K 输入、8K 共享前缀、16 输出 Token。RDMA 条目采用本轮双 netns + SIW 跨实例 Qwen TTFT；其余条目保留各自已审计的对照组。

- **前缀复用与首 Token**：显存受限导致共享前缀重复 Prefill → 通过 vLLM V1 异步匹配、批量导入和后台发布复用 CPU KV → TTFT P50 **5.647 → 3.287 s，下降 41.8%**，输出 Token ID 与冷算一致。
- **CUDA 数据路径**：离散 KV 的逐段提交开销高 → 按物理页与 stride 实现 `uint4` Gather/Scatter、pinned 批量搬运及 Event 交接 → 288 MiB 回传的 H2D payload 拷贝 **18,432 → 4 次**、Host 提交＋同步 **432.2 → 30.6 ms**；72 MiB 核函数单向有效载荷带宽 **119.13 / 103.54 GiB/s**。
- **NVMe 回源**：16 KiB 逐层记录串行读取阻塞首 Token → 以 `O_DIRECT + io_uring` fixed buffer 承载校验后的 owned snapshot，按深度并发读取并为后台写回预留容量 → 同负载 TTFT P50 **4.961 → 3.675 s，下降 25.9%**，load/save error 均为 0；独立文件对照中避免 **160 MiB Page Cache** 驻留。
- **跨实例 RDMA 以传代算**：跨实例重算慢→聚合收发与边收边H2D→TTFT降36.9%，KV就绪吞吐达链路上限62.5%；双netns+SIW软件RDMA隔离，超时断链回退且Token一致。

数字来源与边界见 [原性能审计报告](PERFORMANCE-ATTRIBUTION-AUDIT.md)与[跨实例 RDMA 对账](RDMA-CROSS-INSTANCE-AUDIT.md)。前三条对应各自历史审计对照组，第四条来自本轮 Qwen 真实跨实例回放；不混用不同轮次的冷重算数值。RDMA 的 62.5% 包含控制、校验与 H2D，纯负载接收活动的 97.0% 不能替代它。
