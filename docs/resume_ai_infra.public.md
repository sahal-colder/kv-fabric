# AI Infra 求职者

**求职意向：AI Infra 研发实习生｜大模型推理引擎 / CUDA Kernel / KV Cache**
项目代码：https://github.com/sahal-colder/kv-fabric ｜ https://github.com/sahal-colder/nanovllm

## 教育背景

- **硕士研究生**｜电子信息（控制工程）硕士｜2025.09 - 2028.06（预计）
- **工学本科**｜飞行器控制与信息工程本科｜2021.09 - 2025.06；2023-2024 学业二等奖学金

## 个人技能

- **系统与语言**：C11 / Linux、多线程、TCP / RDMA rsockets、租约 / 引用计数、有界内存池、`O_DIRECT` / `io_uring` / SPDK、VFIO；掌握 Python，熟悉 C++17；使用 CMake / CTest 组织构建与回归。
- **CUDA 与推理**：Warp Shuffle、向量化访存、Online Softmax、GQA / MQA、Paged KV、Stream / Event、CUDA Graph；使用 `nsys` / NVTX 归因，以 `ncu` 核验 DRAM 吞吐和 Occupancy。

## 项目经历

### KV-Fabric｜vLLM V1 分层 KV Cache 与 CUDA 数据路径

**技术栈**：C11 / CUDA / Python / vLLM V1 / 物理 NVMe / SPDK / ext4｜个人项目

RTX 4060 Laptop 8 GiB、Qwen2.5-3B BF16；16K / 50% 共享前缀、16 输出 Token，统一 eager / batch-invariant；每格预热 3 次、测量 15 请求。

- **引擎集成与生命周期**：Scheduler 异步匹配前缀，Worker 经 C ABI 完成整批装载门控、分页导入及后台写回；以发布代数、块版本、租约和引用计数保护复用，超时或校验失败回退 Prefill。
- **`nsys` 归因与流水线**：将独立 Device 快照经 Event 交接 D2H 流与发布线程；H2D 调用 **18,432 → 4 次/请求**，Host 提交与同步 **432.2 → 30.6 ms**，D2H 与模型计算重叠 **99.4%**。
- **首 Token 优化**：批量搬运及后台发布后，16K CPU 热回源 TTFT P50 **3.287 s**，较逐段 Connector 的 25.657 s 降 **87.2%**，较原生重算 5.647 s 降 **41.8%**。
- **向量化 Gather / Scatter**：按物理页与 stride 打包离散 KV，校验 16-byte 对齐、末维连续及尾批；`ncu` DRAM 利用率 **90.26% / 90.04%**，实测 **214.99 / 187.66 GiB/s**。
- **物理 NVMe 与验证**：实现 `O_DIRECT + io_uring` 固定缓冲、CRC32C 与旧槽防读；72 MiB 回源 **P50 58.83 ms、有效吞吐 1.195 GiB/s**；16K fixed / POSIX TTFT **5.006 s / 5.034 s**。历史 345 条请求 Token ID 对照通过；当前 26/26 CTest、完整 vLLM 回归及 CPU/CUDA 严格 RDMA 各 30 轮通过。
- **SPDK 裸盘接入**：以 VFIO 接管 NVMe 并适配 39 位 IOMMU；独立 **16 MiB SPDK→GPU 回源 P50 18.95 ms**（30 轮，含 CRC、Staging 与 H2D），逐字节及推理 Token 校验通过。

### nano-vLLM-C｜C11/CUDA 推理执行与 Paged Decode Attention

**技术栈**：C11 / CUDA / FP32、FP16、BF16 / CUDA Graph｜同一 RTX 4060 Laptop

- **完整 Decode 执行**：复用自研 C11 模型加载、调度和采样，将 RMSNorm、QKV / RoPE、分页 Attention、O 投影、SwiGLU FFN、残差及 logits 串联；真实 260K checkpoint 的 **4×32 个 greedy Token** 在 CPU / eager / Graph 下严格一致。
- **SGEMV / RMSNorm 优化**：向量加载、Warp Shuffle 与寄存器复用，覆盖非对齐 / 尾部；8192×4096 / B1 SGEMV 的 `ncu` 实测 **230.06 GiB/s**、DRAM 利用率 **96.59%**；RMSNorm 128×4096 P50 **8.19 → 6.14 µs**。
- **Paged Decode Attention**：通过 Block Table 读取非连续页，支持 GQA / MQA、partial tail；以 **64 Token 分段 Online Softmax** 和共享权重合并减少串行依赖。FP32 B1/H16/KV4/D128/S4096 P50 **73.73 µs**；分段 Kernel DRAM **62.60%**、Occupancy **75.86%**。
- **CUDA Graph 协同**：预分配稳定设备缓冲，动态长度 / 页表经设备间接寻址；`nsys` 验证每步 **51 次 Kernel Launch → 1 次 Graph Launch**。4 层随机权重 Decoder（d512、GQA4、B1、seq129）FP16 计算链路 P50 **250.34 → 221.74 µs（降 11.43%）**，不含元数据传输 / 采样。
- **数值与工程验收**：90 项 Attention 随机 / 边界用例；三层 / B2 连续 **83 步**对照 CPU FP64 logits 与全部物理 KV 槽，同图跨页回放；**8/8 CTest、Memcheck / Racecheck 零错误**，保留原始 profiler 报告。

## 学术科研

- **IEEE ICRA 2027 第一作者投稿（在审）**：机器人集群协同导航与控制方向。
