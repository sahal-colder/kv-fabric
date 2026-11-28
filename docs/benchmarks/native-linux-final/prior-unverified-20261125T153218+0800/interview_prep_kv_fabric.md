## 面试深挖自查表

| 面试官可能追问 | 基于源码和实测的回答要点 |
|---|---|
| **23.87 GiB/s 怎么算？为什么是四次 H2D？** | 8,192 个命中 Token 对应 512 个 16-Token 页，36 层 K/V 共 288 MiB。128 页一批，共四次载荷 DMA。用 NVTX 范围内 CUDA API 的关联 ID 找到 GPU memcpy，按 `288 MiB ÷ 11.784 ms` 得到 23.87 GiB/s。分母为 DMA 活跃区间并集，块 ID 等元数据搬运单独标记。见 [Timeline 解析器](../scripts/summarize_nsys.py#L129)。 |
| **为什么用异步 Stream 以后 Overlap 还是 0%？** | 比例按首 Token 前 `DMA 活跃区间 ∩ 模型 GPU Kernel 区间` 除以 DMA 活跃时间。H2D 与 Scatter 整批完成后，调度器才允许该请求执行后缀 Prefill。D2H 等对应 Gather 的 event，本次单并发抓样中没有与模型 Kernel 相交。后台写回移出了前台等待和发布。见 [批量读写](../integrations/vllm/kvfabric_vllm/cuda_pages.py#L50)。 |
| **后台写回时 vLLM 复用原页会不会读错？显存如何约束？** | 在模型流先 Gather 到独立 device staging，再记录 event，写回流等 event 后 D2H。后台等 D2H 完成才读 pinned staging，发布 Future 结束后才复用槽。四个写槽满时跳过本轮写入。该模型 128 页每槽 72 MiB，四写一读约 360 MiB 显存载荷缓冲，另有同量 pinned 载荷缓冲。见 [写回调度](../integrations/vllm/kvfabric_vllm/connector.py#L301)。 |
| **`ncu` 的利用率、带宽、Occupancy 各说明什么？** | Gather / Scatter 的 DRAM 持续峰值利用率为 88.91% / 88.67%，DRAM 实测读写合计为 365.31 / 364.35 GiB/s。Achieved Occupancy 为 94.83% / 90.69%，表示每 SM 活跃 warp 数占硬件上限的比例。全局读平均 29.89/32 B 每 sector，写 32/32 B。高 DRAM 利用率和约 7.1% 的 SM 吞吐利用率支持显存带宽为主要约束。 |
| **16-byte 向量化怎样处理跨页、stride 和尾部？** | 每个 CUDA block 负责一页、一层、一个 K/V 分面，按物理页 ID 与字节 stride 定位，线程以 `uint4` 搬运。校验基址与外层 stride 16 B 对齐、head 行字节数可整除 16、末维连续。尾批按实际页数发射。布局不满足时自动模式走逐层拷贝，显式要求批量时报告错误。见 [算子](../src/integration/vllm_kv_cuda.cu#L5) 与 [布局校验](../integrations/vllm/kvfabric_vllm/cuda_pages.py#L9)。 |
