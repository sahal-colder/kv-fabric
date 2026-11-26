# AI Infra 求职者

**求职意向：AI Infra 研发实习生｜大模型推理引擎 / CUDA Kernel / KV Cache**
项目代码：https://github.com/sahal-colder/kv-fabric ｜ https://github.com/sahal-colder/nanovllm

## 教育背景

- **硕士研究生**｜电子信息（控制工程）硕士｜2025.09 - 2028.06（预计）
- **工学本科**｜飞行器控制与信息工程本科｜2021.09 - 2025.06；2023-2024 学业二等奖学金

## 个人技能

- **系统与语言**：C11 / Linux、多线程、TCP / RDMA rsockets、租约 / 引用计数、有界内存池、`O_DIRECT` / `io_uring`；掌握 Python，熟悉 C++17；使用 CMake / CTest 组织构建与回归。
- **CUDA 与推理**：Warp Shuffle、向量化访存、Online Softmax、GQA / MQA、Paged KV、Stream / Event、CUDA Graph；使用 `nsys` / NVTX 归因，以 `ncu` 核验 DRAM 吞吐和 Occupancy。

## 项目经历

### KV-Fabric｜vLLM V1 分层 KV Cache 与 CUDA 数据路径

**技术栈**：C11 / CUDA / Python / vLLM V1 / 物理 NVMe / ext4｜个人项目

RTX 4060 Laptop 8 GiB、Qwen2.5-3B BF16；下列 TTFT 对应 16K 输入、8K 共享前缀、16 输出 Token。RDMA 条目采用本轮双 netns + SIW 跨实例 Qwen TTFT；其余条目保留各自已审计的对照组。

- **前缀复用与首 Token**：显存受限导致共享前缀重复 Prefill → 通过 vLLM V1 异步匹配、批量导入和后台发布复用 CPU KV → TTFT P50 **5.647 → 3.287 s，下降 41.8%**，输出 Token ID 与冷算一致。
- **CUDA 数据路径**：离散 KV 的逐段提交开销高 → 按物理页与 stride 实现 `uint4` Gather/Scatter、pinned 批量搬运及 Event 交接 → 288 MiB 回传的 H2D payload 拷贝 **18,432 → 4 次**、Host 提交＋同步 **432.2 → 30.6 ms**；72 MiB 核函数单向有效载荷带宽 **119.13 / 103.54 GiB/s**。
- **NVMe 回源**：16 KiB 逐层记录串行读取阻塞首 Token → 以 `O_DIRECT + io_uring` fixed buffer 承载校验后的 owned snapshot，按深度并发读取并为后台写回预留容量 → 同负载 TTFT P50 **4.961 → 3.675 s，下降 25.9%**，load/save error 均为 0；独立文件对照中避免 **160 MiB Page Cache** 驻留。
- **跨实例 RDMA 以传代算**：跨实例重算慢→聚合收发与边收边H2D→TTFT降36.9%，KV就绪吞吐达链路上限62.5%；双netns+SIW软件RDMA隔离，超时断链回退且Token一致。

### nano-vLLM-C｜C11/CUDA 推理执行与 Paged Decode Attention

**技术栈**：C11 / CUDA / FP32、FP16、BF16 / CUDA Graph｜同一 RTX 4060 Laptop

- **完整 Decode 执行**：复用自研 C11 模型加载、调度和采样，将 RMSNorm、QKV / RoPE、分页 Attention、O 投影、SwiGLU FFN、残差及 logits 串联；真实 260K checkpoint 的 **4×32 个 greedy Token** 在 CPU / eager / Graph 下严格一致。
- **SGEMV / RMSNorm 优化**：向量加载、Warp Shuffle 与寄存器复用，覆盖非对齐 / 尾部；8192×4096 / B1 SGEMV 的 `ncu` 实测 **230.06 GiB/s**、DRAM 利用率 **96.59%**；RMSNorm 128×4096 P50 **8.19 → 6.14 µs**。
- **Paged Decode Attention**：通过 Block Table 读取非连续页，支持 GQA / MQA、partial tail；以 **64 Token 分段 Online Softmax** 和共享权重合并减少串行依赖。FP32 B1/H16/KV4/D128/S4096 P50 **73.73 µs**；分段 Kernel DRAM **62.60%**、Occupancy **75.86%**。
- **CUDA Graph 协同**：预分配稳定设备缓冲，动态长度 / 页表经设备间接寻址；`nsys` 验证每步 **51 次 Kernel Launch → 1 次 Graph Launch**。4 层随机权重 Decoder（d512、GQA4、B1、seq129）FP16 计算链路 P50 **250.34 → 221.74 µs（降 11.43%）**，不含元数据传输 / 采样。
- **数值与工程验收**：90 项 Attention 随机 / 边界用例；三层 / B2 连续 **83 步**对照 CPU FP64 logits 与全部物理 KV 槽，同图跨页回放；**8/8 CTest、Memcheck / Racecheck 零错误**，保留原始 profiler 报告。

## 学术科研

- **IEEE ICRA 2027 第一作者投稿（在审）**：机器人集群协同导航与控制方向。
