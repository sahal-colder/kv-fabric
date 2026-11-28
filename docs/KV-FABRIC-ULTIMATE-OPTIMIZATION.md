# KV-Fabric Remote KV 延迟优化

## 工作负载和基线

本轮以 `dev` 的 `02fe2db383bad59dc614d8dd124d21f976ae0869` 为基线。模型使用 `stories15M.bin` 的 FP32 权重，在 RTX 5060 Ti 上执行 CUDA 前向。每次消费请求包含 24 个提示 Token，远程前缀覆盖其中 20 个 Token 和 5 个完整页。每页 6 层，每层 9,216 Bytes，总计 276,480 Bytes。采样固定为 8 个生成 Token、种子 1234、温度 0.7、top-p 0.95 和忽略 EOS。

在同机 TCP 环回的独立预热组中，CUDA 的 Remote TTFT P50 为 **12.67 ms**，Cold 为 **15.67 ms**，下降 **19.1%**。30 轮中 Remote 全部快于同轮 Cold。关闭共同预热的复跑结果为 12.42 ms 对 15.58 ms，下降 20.3%。[正式样本和配对统计](KV-FABRIC-ULTIMATE-TEST-REPORT.md#ttft-结果)

原始记录的 CUDA Cold 与 Remote TTFT P50 分别为 15.56 ms 和 79.58 ms。用原提交、相同模型和参数在当前主机上复测 30 轮，分别为 15.65 ms 和 85.85 ms。复测每个 Remote 样本都实际导入 5 页，生成 Token ID 与同轮的 Cold 和 Local 一致。[原始历史记录](benchmarks/nanovllm-remote-kv-cuda-raw.csv) · [原提交复测记录](benchmarks/ultimate/baseline/cuda-raw.csv)

### 原提交分段观察

原调用链对每页执行一次 `ACQUIRE_PREFIX`，然后按层执行 `FETCH_STREAM`。发布端复制每层之前和之后各访问一次目录。消费端每页还执行一次续后校验和释放。5 页涉及 30 次数据 RPC 和 75 次目录 RPC，未含独立心跳和发布的流量。原实现已经设置 `TCP_NODELAY` 并复用数据 TCP 连接，目录往返的数量仍随页数和层数相乘。

在临时构建中分别对 `import_page`、`mi_remote_import`、`send_layer` 和 `commit_pages` 插入单调计时探针。10 个保持原参数的远程样本给出以下结果。表中数值是每请求合计的最近秩 P50。发布端耗时包含在消费端对应的逐层 RPC 内。

| 进程 | 观测阶段 | 次数/请求 | P50（ms） |
| --- | --- | ---: | ---: |
| 消费端 | 目录查询及 Grant 获取 | 5 | 4.42 |
| 消费端 | 连接获取 | 5 | 0.32 |
| 消费端 | 逐层数据 RPC，含发布端处理 | 30 | 61.88 |
| 发布端 | 复制前目录校验 | 30 | 26.55 |
| 发布端 | 存储快照和 CRC | 30 | 0.11 |
| 发布端 | 复制后目录校验 | 30 | 25.00 |
| 消费端 | 最终目录校验 | 5 | 4.40 |
| 消费端 | Lease 释放 | 5 | 3.86 |
| 消费端 | CPU 重排 | 5 | 0.06 |
| 消费端 | CUDA 页复制及等待 | 5 | 0.40 |

目录校验产生了旧路径的大部分等待。逐页 CUDA 同步和 CLI 的 1 ms 空转轮询也在关键路径上。探针样本和原提交正式复测分开保存。[旧路径分段原始记录](benchmarks/ultimate/baseline/cuda-legacy-profile.csv)

## 新数据路径

1. 预取线程一次向 Directory 申请连续前缀的 Manifest 和逐页 Lease。目录优先选择能覆盖最长下一段的活跃副本，在同一 Worker 无法续接时可选择后续页的其他 Worker，遇到第一个缺口就停止。窗口限制为 8 页和 8 MiB，长前缀按窗口前进。
2. 对同一 Worker 的连续页，仅发送一次 `FETCH_BATCH`。请求携带各页的完整逻辑键、Boot UUID、Lease Ticket 和规范化 Manifest 的 SHA-256。服务端两次验证整组 Ticket，复用一条控制连接，在中间将所有页和层写入同一个连续缓冲。TCP 发送用 `sendmsg` 合并帧头和载荷。
3. 消费端在每个窗口核对帧 CRC32C 和逐层 Manifest CRC32C，再将每页写入私有 Paged KV Sequence。消费端收到数据后执行 `FINISH_BATCH`，在同一目录锁内验证并释放整个窗口的 Lease。只有通过校验的前缀才能交给引擎。任一窗口报传输错误时会销毁本次私有暂存；真正的目录缺页可以保留前面验证过的完整页。
4. CPU 直接将各层解包到已持有引用的本地页。CUDA 路径在预分配的 `cudaMallocHost` 暂存中按 K 和 V 重排，按连续物理页区间各提交一次 K 和 V 的异步复制。5 页物理连续时一共提交两次 H2D 和一次完成 Event。批量上传使用独立的非阻塞 Stream，DMA 等待线程负责 Event 等待。
5. 网络预取和 DMA 完成后都向引擎的 `eventfd` 发信号。调度线程空闲时等待信号或最早的请求截止时间。仍有 Ready 请求时继续前向。DMA 期间页号只存在于请求自己的预留表，Event 确认完成并且截止时间仍有效时才进入本地索引。

目标 5 页工作负载用 1 次目录获取、1 次数据请求、2 次同连接发布端目录校验和 1 次消费端验证加释放。目录监听每次只让一个 lane 等待 accept 队列，其余 lane 可以处理已有连接。数据连接池按地址和 Boot UUID 借出 socket。空闲探测与 FIN 可能交错，只有复用连接在收到任何载荷前断开时，才允许在原截止时间内重新连接一次。

Wire v3 的单页和逐层操作仍然可用。新批操作部署在更新后的 Directory 与 Worker 上。旧对端不理解新操作时，Nano-VLLM 回退到本地前向。超出单页批量上限的合法配置和通用存储 Worker 的传统解析入口保留逐层路径。CUDA 物理页碎片化时，每个连续区间有一对复制提交，整个导入仍只用一个完成 Event。

## 准入策略

引擎用实际提示 Token 的前向时间更新每 Token 成本的 EWMA。一次成功导入提供两个独立的远程观测量。连接、目录、响应首字节等待和调度属于固定开销；正文接收、CRC、重排、DMA 和提交按实际载荷字节数估计单位成本。EWMA 新观测权重为 1/8。

剩余提示为 `R`，可命中完整页覆盖 `H` 个 Token、传输 `B` Bytes 时，引擎将省掉的本地计算估计为 `[R − max(1, R − H)] × prefill_ns_per_token`。最后一个提示位置仍须前向以获得采样 logits。远程成本为 `fixed_ns + B × ns_per_byte`。缺少成功观测时先探测。历史成本超过节省量时直接走本地前向。目录只返回短前缀时用实得页数再判断一次。准入跳过计入 `remote_admission_skips`，网络、Lease 和资源故障另有计数。

## 发布与撤销

- `PENDING` 拥有不可变快照。CUDA 发布端先等产生该页的前向结束，再在仍持有物理页引用时完成 D2H Event。快照交给后台发布器后，发布器只读自己的 CPU 字节。
- `PUBLISHING` 准备整个页的 Paged KV Lease，封存每层并生成 Manifest。目录确认本条 Manifest 后进入 `ACTIVE`。对于 ACK 丢失的尝试，Worker 持有的快照只有在目录能验证其 Ticket 时才可以服务。
- `RETRY` 保持原页身份。已经保存在 Worker 的快照由心跳重试，直到确认、撤销或收到永久冲突；尚未取得 Worker 引用的暂时性故障会有界重试。不可恢复的冲突进入 `FAILED`。
- `RETIRING` 当场关闭本地服务门，并优先把目录撤销排入发布队列。撤销后的迟到发布受取消标记拦截。正在复制的请求保留独立页引用，发送前再查服务门和 Manifest。

目录校验绑定 Worker Boot UUID、Directory Epoch、随机 Secret、递增 Fence 和目录内部 Replica Revision。Manifest 绑定各层的 Block ID、Generation、Version、大小和 CRC。源 Storage 取得 READ pin 后再读快照，消费端先验证后接受，GPU 写入也只指向本请求仍拥有的物理块。已发出的 TCP 字节无法撤回，目录撤销与消费端最终校验按锁内顺序生效。异步撤销 RPC 返回前，目录可能短时间返回旧定位，但源 Worker 已关闭该定位的服务门。

请求取消或超时后，调度线程可以开始本地计算。Connector 按总截止时间结束 I/O 并释放 Ticket。已提交的 DMA 保留页引用和暂存所有权直到 Event 完成，再回收物理页。引擎关闭时依次停止并等待后台线程，再销毁 Stream、Event、Pinned Arena、Storage 和 TCP 连接。

## 存储和对话边界

TLSF 的一级和二级位图、物理相邻双向链表及独立 Host 元数据保持原实现。MiniInfer 的 VRAM、CPU Pinned 和 SSD 迁移仍使用原有后端。SSD 记录回读核对句柄 Generation、块 ID、Version、大小和 CRC。Nano-VLLM 的 TCP 共享使用独立 Worker 缓存，本地 SSD Spill 仍走已有 POSIX、io_uring 或 SPDK 接口。

`--kv-publish-generated` 可将已完成前向的生成 Token 所在完整页纳入发布。服务端可用规范化 Token 历史续接三轮对话。尚未完成前向或未填满的尾页由后续请求重算。`TP=1, PP=1` 继续由命名空间和 CLI 验证。

## 阶段计量

| 字段 | 计量边界 |
| --- | --- |
| `dir_lookup_ns` | 链哈希、目录批查询、Grant 解码和 Manifest 摘要 |
| `conn_acquire_ns` | 数据连接借出、探测或建连 |
| `net_recv_ns` | 数据帧头和正文实际接收等待，含源端处理 |
| `crc_verify_ns` | 消费端帧 CRC 与逐层 Manifest CRC |
| `cpu_stage_ns` | CPU 暂存分配、复制和 K/V 布局重排 |
| `cuda_h2d_ns` | 向专用 Stream 提交整批 H2D 和完成 Event |
| `cuda_sync_ns` | DMA 等待线程等待完成 Event |
| `kv_commit_ns` | Paged KV 预留与提交、物理页及 Nano 前缀索引更新 |
| `sched_resume_ns` | 完成信号到调度收割，以及导入完成到首次恢复前向 |
| `lease_check_ns` | 消费端最终校验和 Lease 释放 |
| `prefetch_queue_ns` | Connector 和 DMA 等待线程的入队至运行 |
| `first_decode_ns` | 最后提示位置开始前向到首次采样结束 |
| `ttft_ns` | 开始提示编码到首次采样完成 |

上述细分覆盖 Nano-VLLM 批量 Connector。逐层兼容路径仍保留原逐页累计指标。各阶段使用同一 `CLOCK_MONOTONIC`，耗时区间互不叠加。源端验证时间已经包含在接收等待中。批量路径中的逐页兼容计数代表该页的校验和 CPU 暂存时间，网络时间按整批记录。超时回退可与后台清理并行，故障样本的后台耗时可能延伸到 TTFT 之后。
