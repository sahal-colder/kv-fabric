# 异步 NVMe I/O 与迁移设计

## 阶段与兼容性

先实现单 Reactor SPDK，保留 `mi_spdk_transfer()`；增加 submit/wait/cancel/batch 和统计。
`mi_spdk_config`、`mi_storage_config` 的已有字段及布局不变；新 options 通过新入口传入。
随后迁移 worker pool、GPU 独立 Stream/Event、io_uring 批量、统一记录与生产路径 benchmark。
物理硬件测试单独执行，不加入默认 CTest。

## SPDK 请求生命周期

`FREE -> QUEUED -> ACTIVE -> [IO_WAIT -> ACTIVE] -> COMPLETE -> FREE`。
请求对象和 DMA 均在 open 时预分配，整个请求独占一槽；最大实际 bdev 在途数不超过 QD。
提交者在短 mutex 内借槽及入队；零周期 Reactor poller 分批取请求，所有 SPDK API/channel
只在拥有它的 SPDK Thread 执行。每请求保留 range、position、phase、DMA、wait entry、错误与取消位。
完成回调释放 bdev_io，推进 chunk 或发布完成。等待者只等待自己的请求，读缓冲在成功完成后可见。

cancel 为协作取消：未开始请求不发 I/O，已开始请求等待当前 DMA 完成，不再发后续 chunk。
不声称撤销已写入字节。close 拒绝新提交，排空已接受请求，等待调用方释放完成票据后释放池。
若驱动永远不给 completion，close 保持等待，不能猜测 DMA 已停。调用方须停止创建新操作后 close。
保留不对齐兼容操作的 RMW；并发重叠写入由调用方排他，生产记录使用完整对齐 I/O。

可配置 QD 1..128、DMA 槽数、最大传输长度、每轮提交批量；Reactor mask 沿用配置；
bdev/iobuf 资源池由 SPDK JSON 管理并在报告中记录。统计区分逻辑请求与真实 bdev 提交/完成。

## 迁移与 GPU 流水线

`max_async_migrations` 改为所有迁移的运行上限；待执行队列容量另设为上限的两倍。
worker pool 复用原来的 prepare/copy/commit/rollback 实现；直接同步调用也领取运行令牌。
同块仍以 busy/lease 排他。batch 使用有界票据窗口，不创建每 Page 一个线程。
保留源和目标直到 GPU Event 和 SSD completion 均确认；只有复制/CRC 成功才变更驻留位置。

GPU backend 的固定 transfer 槽各自拥有 Stream/Event。不同 worker 的独立 Page 能重叠
D2H/SSD 或 SSD/H2D；单 Page 内仍按依赖顺序执行。共享 SPDK DMA/CUDA host-register
不能未经驱动验证开启；先使用安全 pinned 多缓冲，明确记录额外 memcpy，后续原型单独报告。

## io_uring 与记录格式

ring 由唯一引擎线程持有，多个调用方通过有界请求池并发；批量 SQE、CQE drain、每请求进度。
DIRECT 的所有 staging 槽共享一个 Ring owner，使用内部注册的多 Buffer Pool；不再每槽创建 Ring/线程。
Fixed buffers/files、IOPOLL/SQPOLL 通过显式 options 启用；失败返回错误，绝不以未启用配置标注启用。
不确定 submit/wait 失败先同步 cancel，无法证明停止则隔离 ring-owned DMA。

对齐记录为 4 KiB metadata + round_up(payload,4096)，记录 magic/版本、ID、Version、Generation、
payload CRC 和 header CRC。完整写入且可选读回成功后才在内存槽表发布。缓存后端无持久索引，
不声称 power-fail 原子性；Raw A/B 使用相同 non-durable completion 或相同 flush 边界。
保留旧 buffered 文件 API 的紧凑布局以免改变已有容量预算；新的对齐生产路径用于公平对照。

## 证据

A Raw：相同数据/偏移/QD/CPU/NUMA/持久化边界，使用生产引擎；记录 actual high-water。
B Migration：包含 CRC、记录、copy、GPU，分别报告总时长与各阶段。
C Inference：独立记录冷/热缓存、SSD 回源、TTFT，不能与 A/B 混算。
每组 >=3 warmup、>=30 samples，交替顺序；保留 CSV、配对汇总、环境与源 SHA-256。

## 网络与存储边界（架构纠正）

网络 `src/network/reactor.c`（epoll）、`proactor.c`（网络 io_uring）、`ntyco.c` 保持独立，
`src/main.c:start_network()` 的配置选择、协议/TCP/复制/RDMA 不改动。
本文 Reactor 只指 SPDK 后端内部执行线程；存储 io_uring 位于 `src/worker/uring_io.c`，
不复用网络 Proactor。迁移 worker 只协调 Block 与 CUDA/SSD 依赖，不能操作 SPDK channel。
最终回归分别运行三种网络构建/测试以及 SSD 两后端；不能以 SPDK 测试替代网络回归。

## 同盘对照与记录兼容补充

新增 raw-uring 区域后端只打开已有、未挂载块设备（O_EXCL/O_DIRECT），不创建文件、截断或格式化。
SPDK 与 raw-uring/DIRECT 均使用 MIFSSD3。旧 POSIX/buffered-uring 文件 API 保留 MIFSSD1 的容量/格式兼容，
不参与物理 Raw 与对齐记录 A/B。MIFSSD3 读取拒绝 MIFSSD1/MIFSSD2，不自动恢复旧持久索引。

推理 CLI 增加 raw-uring 和显式设备、区域起点、I/O CPU 参数；原 posix/uring/spdk 模式保留。
SPDK 小 payload 的容量按 4096 + round_up(payload,4096) 修正，不能再套用 64 B header 的公式。

128 个 4 MiB DMA 槽需 512 MiB，加上 SPDK runtime 后超过原 512 MiB 大页预留。
硬件 runner 根据矩阵最大 DMA 计算大页需求（完整 Raw 为 768 MiB），必要时做一次内存压紧，恢复原计数；
双方整个 A/B 期间均处于相同大页预留状态。
配置 bdev_io_pool_size=8192、cache=256、large iobuf=256，完整实际 JSON 写入每次实验目录。

完整对齐 Page 使用 4 KiB Header 与原 payload 的 gather/scatter，直接到 Request DMA，不做中间整页 memcpy。
严格读回或非整 Page 继续借预分配记录池，不能以关闭 CRC 换取速度。
