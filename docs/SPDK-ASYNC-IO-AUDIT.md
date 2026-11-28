# SPDK 数据通路源码审计

审计日期：2026-11-28，基于开始任务时的工作区（包含原有未提交修改），不是仅基于 HEAD。
基线文件 SHA-256：`benchmarks/spdk-async-2026-11-28/baseline-sha256.json`。
下述行号均指该基线；最终代码位置见实施报告。

| 代码位置 | 实际行为与影响 | 具体改造 | 可验证效果 |
|---|---|---|---|
| `src/worker/spdk_bridge.c:30-41,273-292` | 全设备一个 range/position/done/DMA；submit mutex 从提交持有到完成；多个调用方实际 QD=1 | 预分配独立请求及 DMA 槽；短锁入队；Reactor 拥有 channel；逐请求完成通知 | bdev 成功提交减完成的最大值 >1；QD 上限生效 |
| `spdk_bridge.c:18,68-134` | 256 KiB chunk，512 KiB+64 B 记录至少三段；回调才提交下一段 | 可配置最大传输，默认 1 MiB；请求间流水提交 | 512 KiB Page 一次对齐记录 I/O（设备可内部拆分） |
| `spdk_bridge.c:97-115` | 全局一个 bdev_io_wait_entry；无法支持多个资源等待者 | 每请求独立 wait entry；ENOMEM 保持请求与 DMA 所有权 | 资源池不足可重试，不重用 Buffer |
| `spdk_bridge.c:141-167,296-306` | close 依赖 submit 锁证明没有在途操作；无并发请求关闭模型 | 停止接收、排空队列、等待所有 callback 和 wait entry，再关闭 channel/desc/DMA | 关闭与完成竞争安全；取消不能提前回收 DMA |
| `tiered_storage.c:417-505` | 同步迁移本身已在 busy 标记后释放 Storage 锁，独立 Block 可以由外部线程并发；不是全局锁覆盖磁盘 I/O | 保留两阶段提交与 rollback；增加全局迁移并发令牌 | 同块冲突仍返回 EBUSY；独立块实际同时执行 |
| `tiered_storage.c:508-518,530-594` | batch 串行 for；一个 worker；queued 包含执行中 job，max_async_migrations 原为总容量 | 有界 worker pool；取走 job 时出队；batch 滑动窗口；运行上限与排队容量分别统计 | batch 32/64/128/256 复用生产迁移实现 |
| `tiered_storage.c:396-414` | 每任务可借独立 pinned 槽，但 GPU copy_owned 使用默认 stream，同一设备默认流会限制重叠 | GPU transfer 槽使用独立 Stream/Event，完成确认后交给 NVMe | 跨 Page D2H 与 SSD、SSD 与 H2D 重叠；保留失败隔离 |
| `storage_backend.c:24-27,316,552-555,617-689` | MIFSSD1 64 B header；payload 非 4 KiB 对齐；SPDK 首尾 RMW；publish 的 16 KiB checksum 反复读盘 | 新的版本化对齐记录；完整 Page CRC；write_record 从源计算；独立严格读回 | 正常整 Page 无 RMW；旧 magic 明确失败 |
| `storage_backend.c:202-277; direct_io.c:18,58-83,126-168` | buffered uring 一个 ring 单请求；direct 路径已有 4 KiB header、fixed buffer、每槽 Ring 并发（不能称为所有 uring 都 QD1） | 为通用 uring 增加批量 SQE/CQE 与实际 QD；保留 direct fixed 路径；明确 flags/文件注册状态 | 不人为降低对照组；同设备 Raw 对照走生产引擎 |
| `uring_io.c:31-34,117-223` | 一个 buffer，mutex 覆盖 submit/wait；depth<=64 但一次一个 SQE/CQE | 引擎线程独占 ring；请求池、Buffer Pool；批量 submit 与 CQ drain；严格取消隔离 | 实际 QD 与 submitted/completed/high-water 可观察 |
| `paged_kv.c` | Page Lease/Version 与 Host 元数据、Device 地址分离；迁移应继续走 Storage，不另写 benchmark 快路 | 保持所有权边界；扩展 Storage 回归覆盖批量 | 原有 Prefix、远程与推理回归 |
| `tools/kvf_spdk_verify.c; tools/kvf_nvme_recovery_bench.c` | 以串行迁移为主；后者 15 样本，不能证明 QD；strict 验证和计时语义需要明确 | 生产接口的并发 bench，至少 3 warmup/30 样本；A/B/C 分层 | 保留所有样本，不展示最好一轮 |
| `scripts/test_spdk_nvme.py; scripts/spdk_device.py; config/spdk-samsung.json` | 已有原始区域备份/恢复及 VFIO 绑定检查；JSON pool=1024/cache=64 | 保留身份、mount/swap/holder 检查与 SHA；显式资源池；硬件不注册默认 CTest | 物理同盘顺序切换，失败也恢复 |

## 正确性约束

提交成功至 completion 之间 DMA Buffer 属于设备；ENOMEM wait entry 也持有请求。
取消只请求停止后续传输，必须等待已提交部分完成。关闭不能强制 free 未确认资源。
旧公开配置结构保持大小，扩展使用新增 API/options；新记录格式不能静默当成旧格式读取。
CRC、Block ID、Version、Generation、Lease 和 busy 继续参与提交判定。

## 性能结论边界

上述是源码瓶颈判断，不是测量出的加速比。历史 `backend_compare` 自有 SPDK 快路的数字不能
替代此次生产 `mi_spdk_*` 引擎的验收证据。malloc bdev 可验证并发和资源生命周期，不能代表物理 NVMe。
