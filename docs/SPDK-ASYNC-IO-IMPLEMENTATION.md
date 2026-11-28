# SPDK / SSD 异步数据通路实施记录

日期：2026-11-28。改造起点为用户已有大量未提交修改的工作区；未 reset、checkout、提交或覆盖其他模块。
基线与变更 SHA 清单位于 `benchmarks/spdk-async-2026-11-28/`。性能数字见独立 benchmark 报告。

## 网络边界

`src/network/reactor.c`、`proactor.c`、`ntyco.c`、`src/main.c:start_network()` 保持原有实现和配置选择。
网络 Proactor 的 Ring 与 SSD 的 Ring 没有共享。SPDK Thread 不接受 TCP 连接，也不运行迁移任务。
`network-sha256.json` 用于核对网络源码未被本轮改变；现有 `kvs_connection.c` 的用户修改也保持原样。

## 修改与目的

| 文件 | 具体改变 |
|---|---|
| `src/worker/spdk_bridge.c:16`、`.h` | 独立预分配 Request/条件变量/DMA/wait entry，短锁入队、单 SPDK owner 批量提交；移除覆盖整个请求的 submit 锁 |
| `spdk_bridge.c:98,154` | 每请求 ENOMEM 重试、完成推进 chunk；实际 bdev submit/complete/high-water、排队和 pool 等待计数；默认最大 chunk 1 MiB |
| `spdk_bridge.c:298,350,377` | 多调用方 submit/wait、协作 cancel、无池死锁的 batch；close 拒绝新提交并等待回调及所有票据被消费 |
| `src/worker/uring_io.c:56,188,231,245` | 单 Ring owner、多请求池、多 Buffer、批量 SQE/CQE、短完成推进、EINTR、注册文件与 Fixed File；显式 IOPOLL/SQPOLL，失败不标启用 |
| `src/worker/direct_io.c`、`.h` | DIRECT 所有槽共享上述 Ring，注册多 Buffer Pool，保留每 GPU 槽 Stream/Event；保留明确标识的 POSIX 回退；硬件 A/B 拒绝回退 |
| `src/worker/io_transfer.h`、`io_metrics.h` | 内部异步/批量接口、每请求 enqueue/dispatch/实际完成时间和 copy 时间；不添加 ops/vtable/通用 ctx 分发 |
| `src/worker/storage_record.h` | MIFSSD3：4 KiB metadata + 对齐 payload；显式格式版本、ID、Version、Generation、payload/header CRC32C |
| `src/worker/storage_backend.c` | SPDK 整 Page staging Pool；正常 write_record 不再走 64 B header/RMW/16 KiB checksum 读回；新增已有 raw block 区域的 io_uring 后端；阶段统计 |
| `include/miniinfer/storage_backend.h` | 新增 options/create_ex/raw 区域入口和 metrics；旧 config 结构布局不变，旧入口保留 |
| `src/worker/tiered_storage.c:421,517,534,585` | 有界迁移运行令牌、worker pool、取出即出队、batch 窗口、取消后的安全回滚；队列容量=2×运行上限 |
| `include/miniinfer/tiered_storage.h` | 保留旧结构布局；明确 max_async_migrations 的运行上限语义，新增运行/排队统计与 cancel |
| `storage_backend.c` GPU copy | 固定 transfer 槽各有 Stream/Event；确认 GPU 完成后推进 SSD；多 Page 间可重叠，失败继续隔离源、目标及 staging |
| `src/nanovllm/cli.c`、`nano_cache.h`、`nano_spill.c` | 原模式保留，新增 raw-uring 显式区域选项；修正 SPDK 小 Page 容量；记录推理调度同步等待 SSD restore 的时长 |
| `tools/kvf_async_bench.c` | Raw 测试直接调用生产 SPDK/uring submit/wait；实际 QD、完成时间分位数、PMU cycles、进程 CPU、copy、独立 CRC 校验 |
| `tools/kvf_migration_bench.c` | 生产 Storage batch CPU/GPU Spill/Recovery，CRC 留在读写路径；严格字节检查置于额外验证区间 |
| `tools/kvf_spdk_pinned_probe.c` | 独立内存注册原型，无 NVMe 快路；SPDK 大页 DMA 与 CUDA 注册及往返验证 |
| `scripts/run_spdk_async_ab.py`、`run_spdk_async_suite.py`、`summarize_spdk_async.py`、`summarize_spdk_migration.py`、`render_spdk_async_report.py` | 同盘保护、交替顺序、全部样本、配对汇总、环境/源码 SHA、原始区域 SHA 恢复；A/B/C 分层 |
| `config/spdk-samsung.json`、`scripts/spdk_device.py` | bdev pool=8192/cache=256、large iobuf=256；Reactor mask 沿用显式配置；区域与设备身份保护保留 |
| `src/worker/CMakeLists.txt` | 可选构建工具与默认 malloc/CPU 回归；没有把物理设备测试加入 CTest |
| `tests/worker/test_async_io.c`、`test_spdk_faults.c`、`test_migration_parallel.c`、`test_uring_faults.c` | 并发窗口、池耗尽、取消、关闭、提交/完成失败、资源唤醒、移除事件、256 Page 并行、队列满、活动/排队迁移取消、部分完成、失败 completion 后 close/票据消费竞争 |
| `test_spdk_lifecycle.c`、`test_storage_audit.c` | 更新为 poller 无需业务线程消息的语义、QD128 边界，保留既有错误/CRC/Lease/回滚检查 |
| `tests/kvstore/test_sync_e2e.py` | 测试识别 NtyCo 原有 SIGTERM 退出码（与已有 reliability 测试一致），不改网络退出实现 |

## 生命周期和兼容边界

* 同步接口内部提交独立请求并等待。异步 range 借到 wait 返回；一张票据恰好一个消费者。
* ENOMEM wait entry 持有 Request/DMA，取消不会从回调下面回收它。请求取消返回 EBUSY，已经写入的字节不会被声称撤销。
* 关闭前须停止前台生产者。close 排空已接收操作，并等待其所有者消费票据；驱动不回 completion 时保持等待，不按超时猜测 DMA 停止。
* io_uring 的模糊 submit/wait 错误先尝试同步取消。一个 CQE 只能证明一个 I/O 完成；无法证明全部停止时，保留 Ring-owned DMA 和资源，拒绝后续使用。
* Storage 在 busy 标记后放锁复制；Lease/Version/Generation 仍排他。成功复制/CRC 后提交驻留位置；取消或失败保留源，释放目标失败进入 cleanup 句柄流程。
* 迁移阶段计时是并发阶段累计 wall time，不能简单相加作为端到端耗时。推理的 ssd_restore_wait_ns 是调度同步等待，不能当作 GPU 硬件 idle counter。
* 旧 POSIX/buffered-uring 文件 API 继续使用紧凑 MIFSSD1 以保留原容量契约。对齐的 SPDK、DIRECT/raw-uring 使用 MIFSSD3，明确拒绝 V1/V2；不声称提供旧持久索引迁移。
* 缓存记录在内存槽表中完成后才可读。Raw A/B 使用相同非持久完成语义；不声称单次大记录写有掉电原子性。

## 测量版本与可复现性

Raw 全矩阵完成后，公平性复核又去掉了 DIRECT/io_uring 的 CPU 整页中间拷贝。
Raw 不链接该记录模块；重新编译后的 Raw 可执行文件与实测文件 SHA-256 完全一致：
`0d25cb9d29988b390ceea6acf505f6ed0090e3d8301d110414b9363ef2505fdd`。
原 Raw 源码逐文件校验归档在其 `source-snapshot/`，最终报告既验证该归档，也验证最终 Raw 二进制一致。
迁移和推理使用最终记录层全部重新测量，旧中断样本保留在诊断目录，不参与汇总。

## GPU 与共享 Buffer 原型

每个 Page 内按 D2H→SSD 或 SSD→H2D 排序；独立 Page 用不同槽/Stream/Event，在多个迁移任务间重叠。
完整对齐 Page 通过 Header/Payload gather/scatter 直接进入请求 DMA 槽；SPDK 与 io_uring 的 CPU 整页路径均去掉中间整页 memcpy，GPU 路径均为 pinned→DMA。
非整 Page、旧 partial publication 和严格读回仍使用预分配记录池；额外 memcpy 全部纳入统计。
`pinned-probe.log` 已验证 1 MiB SPDK 大页 DMA 可 CUDA Host Register，H2D/D2H 字节往返正确。
该原型没有验证 NVMe 与 CUDA 对同一槽同时参与的完整生命周期，因此没有据此启用生产共享池。

## 配置示例

```c
mi_spdk_options options = {
    .queue_depth = 64, .buffer_count = 64, .submit_batch = 32,
    .max_transfer_bytes = 1024 * 1024
};
mi_ssd_spdk_backend_create_ex(config, options, 512 * 1024,
                               MI_SSD_VERIFY_ON_READ, &backend);
```

QD 支持 1..128；DMA 槽必须不少于 QD，最多 512；最大 chunk 按 4 KiB 对齐，最大 4 MiB。
Reactor mask 使用原 config；bdev/iobuf 池大小通过实际 SPDK JSON 配置。DMA 预算为槽数×最大 chunk。
`mi_uring_create_ex` 明确选择 fixed buffers/files、IOPOLL/SQPOLL、owner CPU，并通过 stats 报告实际启用值。
SQPOLL 不重复计算已发布 SQE，暂时 SQ 满会回队重试；额外 3 个 SQPOLL CTest 各重复 20 次通过。
DIRECT staging 满时等待槽归还，不会把运行并发大于 I/O QD 的正常情况错误地当成迁移失败。

## 回归与局限

Release/CUDA/SPDK：48 项；ASan+UBSan/CUDA/SPDK：48 项；TSan/SPDK：43 项，全部通过，日志均在证据目录。
ASan/CUDA 使用 `ASAN_OPTIONS=protect_shadow_gap=0` 兼容本机 CUDA 虚拟地址映射，未关闭堆栈越界检测或项目校验；原不兼容运行日志保留。
TSan 曾指出故障注入测试 mode 的跨线程访问，已改为 C11 atomic；未加入 suppressions。
此主机 GCC TSan 直接运行出现 unexpected memory mapping，`setarch x86_64 -R` 后全部通过；
原失败日志也保留，没有关闭检测器或把失败写成通过。
三个网络模型独立构建并通过 reliability；NtyCo 额外通过认证复制全同步/重连测试。

SPDK 资源不足与设备移除的故障覆盖包含编译期静态 wrapper 注入；不等同于实际热拔盘。
机器无 `/sys/class/infiniband`，没有新增物理 RDMA 往返成绩；保留现有 TCP/远程/传输故障回归。
多 Reactor、GPU/NVMe 生产共享池、掉电恢复、真实热拔盘属于未验证范围，不能由本次模拟故障推断通过。

硬件 runner 校验 JSON 的 PCI 地址与已核验控制器一致，恢复时重新检查 unused 并使用 O_EXCL；
信号退出会等恢复结束，不能在子 runner 恢复之前退出套件进程。
256×512 KiB 记录需要 129 MiB，额外 1 MiB 单独 fsync 备份及目录，纳入前后 SHA 校验。

## 复现

```bash
cmake -S . -B build-spdk -DCMAKE_BUILD_TYPE=Release \
  -DMINIINFER_WITH_URING=ON -DMINIINFER_WITH_SPDK=ON \
  -DMINIINFER_WITH_CUDA=ON -DNANO_WITH_CUDA=ON
cmake --build build-spdk -j8
ctest --test-dir build-spdk --output-on-failure -j8
cmake -S . -B build-async-spdk-asan -DCMAKE_BUILD_TYPE=Debug \
  -DKVF_SANITIZE=ON -DMINIINFER_WITH_SPDK=ON -DMINIINFER_WITH_URING=ON \
  -DMINIINFER_WITH_CUDA=ON -DNANO_WITH_CUDA=ON -DINFER_SPDK_ROOT=/opt/spdk
cmake --build build-async-spdk-asan -j8
ASAN_OPTIONS=protect_shadow_gap=0 ctest --test-dir build-async-spdk-asan --output-on-failure -j8
cmake -S . -B build-async-spdk-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DMINIINFER_WITH_SPDK=ON -DMINIINFER_WITH_URING=ON -DINFER_SPDK_ROOT=/opt/spdk \
  '-DCMAKE_C_FLAGS=-fsanitize=thread -fno-omit-frame-pointer -fno-pie' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread -no-pie'
cmake --build build-async-spdk-tsan -j8
setarch x86_64 -R ctest --test-dir build-async-spdk-tsan --output-on-failure -j8

# 仅本机已核验序列号/备份区域；其他主机必须先建立自己的保护清单，不能照搬设备。
sudo python3 scripts/run_spdk_async_ab.py --out /absolute/new/raw-result
sudo python3 scripts/run_spdk_async_ab.py --layer migration --out /absolute/new/migration-result
sudo python3 scripts/run_spdk_async_ab.py --layer inference --out /absolute/new/inference-result
python3 scripts/summarize_spdk_async.py /absolute/new/raw-result
```

## 最终硬件结果（A/B/C 分开）

完整 Raw 980 配置、迁移 280 配置、推理 132 次进程运行完成。每配置3预热/30测量，两个交替顺序批次。
最终入口 `benchmarks/spdk-async-2026-11-28/final-v4/`，Raw 指向归档且二进制完全相同的 `final-v3/raw`。
所有 Raw 配置的真实提交-完成高水位都达到指定 QD 1/4/8/16/32/64/128；迁移运行数也达到128。

* 4 KiB 随机读 QD128：SPDK 173,077、io_uring 176,079 IOPS，峰值接近。
* 512 KiB 顺序读 QD32：2108.2 / 1941.0 MiB/s；P99 7614 / 8728 µs；SPDK cycles/I/O 约为4.5倍。
* 128×512 KiB GPU Recovery 并发16：1.807 / 1.627 GiB/s，P99 batch 37.396 / 45.356 ms。
* 推理 SSD 回源 TTFT 中位：0.167 / 0.269 ms，属于2层 dim=32 CUDA fixture；cold-B含首次io_uring owner启动，不能作为纯SSD加速比。

完整所有大小/QD/负载分组、CPU、P50/P95/P99/P99.9、阶段时间、配对统计与限制见 `SPDK-ASYNC-IO-BENCHMARK.md`。
物理备份的 first/scratch/last 及迁移扩展区域均恢复SHA一致；大页恢复256、NVMe poll_queues恢复0、VFIO服务恢复active。

## 额外流水线证据与轮询模式

最终IOPOLL/SQPOLL各48个物理配置通过，flags真实启用且达到QD128；所有区域SHA、poll_queues=0及VFIO状态恢复。
`tools/kvf_migration_trace.c`和`scripts/profile_spdk_async.py`只给原生产迁移调用添加NVTX区间，原benchmark对象和生产库直接链接，无另写SPDK快路。
Nsight Systems记录真实CUDA copy执行并与另一Page的SSD未完成调用区间求交；两方向均可观察跨Page重叠。
SPDK D2H/H2D被其他Page SSD调用覆盖约94.59%/93.74%，io_uring为93.92%/93.97%；分母为CUDA执行时间。
SSD区间含排队、copy及完成等待，这不是纯NVMe DMA的重叠百分比；完整口径、CSV、SQLite、nsys-rep见benchmark报告。

## 独立网络模型复现

```bash
make -j8 BUILD_DIR=build-async-network-reactor PROACTOR=0 NTYCO=0 all
KVS_TEST_BUILD_DIR=build-async-network-reactor KVS_TEST_NETWORK=reactor \
  python3 tests/kvstore/test_reliability.py
make -j8 BUILD_DIR=build-async-network-proactor PROACTOR=1 NTYCO=0 all
KVS_TEST_BUILD_DIR=build-async-network-proactor KVS_TEST_NETWORK=proactor \
  python3 tests/kvstore/test_reliability.py
make -j8 BUILD_DIR=build-async-network-ntyco PROACTOR=0 NTYCO=1 all
KVS_TEST_BUILD_DIR=build-async-network-ntyco KVS_TEST_NETWORK=ntyco \
  python3 tests/kvstore/test_reliability.py
KVS_TEST_BUILD_DIR=build-async-network-ntyco KVS_TEST_NETWORK=ntyco \
  python3 tests/kvstore/test_sync_e2e.py
```

上述各命令独立验证原network配置入口，没有将网络Proactor与SSD Ring合并。
`final-v4/task-files.json`逐文件列出本任务41个修改/新增文件及SHA；可取得基线的文件同时附基线SHA。
`final-v4/final-validation.json`记录所有硬件阶段恢复及测试校验；`toolchain.json`补充编译器、CUDA、Nsight、liburing和CPU拓扑版本。
