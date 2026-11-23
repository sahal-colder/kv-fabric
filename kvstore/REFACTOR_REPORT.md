# KVStore 重构报告

2026-11-10

## 架构与范围

保留 Reactor、Proactor、NtyCo 三个网络后端，Array、RBTree、Hash、Skiplist 四个索引，以及 libc、jemalloc、自研池三种分配器。改动基于开始时的现有工作目录，其中已包含未提交的配置和复制改造。仓库远端只有 `origin/main`，基线 HEAD 为 `9a8f1fbd3edf3663380abfbe47c63759bd303226`，压测使用当时工作目录源码构建的二进制。

1. Network 持有连接和输入输出缓冲区。Protocol 对文本按 LF、对 RESP 按长度分帧，在输出达到批次上限后暂停解析。
2. RESP 与文本进入同一 Engine 变更路径。写锁覆盖索引变更、AOF 追加与复制发布，读路径在锁内消费借用的值。
3. Storage 持有一套在线索引。AOF 回放和快照导入在临时索引上构建完整结果，成功后交换所有权。
4. Replication 在同一存储锁的保护下获取全量快照并建立增量序号边界。独立发送线程处理 I/O，有界队列和超时隔离慢副本。连接恢复后重新进行全量同步。
5. Memory 继续通过统一分配接口工作。自研池保留现有尺寸类和共享互斥锁，清零移出锁区。

删除了文本协议的四份 CRUD 分派、事件来源 TLS 和独立回放分派、连接驱动回调表、Reactor 与 Proactor 按 fd 索引的百万项连接表、重复的 RESP 分帧器、同步等待文件 CQE 的持久化 ring。

## 修复与代码入口

| 范围 | 源码确认的问题与修复 | 主要文件 |
| --- | --- | --- |
| Network / Protocol | Reactor 使用阻塞 socket，读到文本半包即执行，且可能在无输出时等待可写。现在按帧执行，使用非阻塞 socket、按需缓冲和写偏移。Proactor 对每个连接只保留一个在途操作，检查提交结果，在关闭时回收所有 CQE 再释放缓冲。 | `src/network/`、`src/protocol/` |
| Engine / Storage | 文本变更释放锁后才追加和发布，可能跨越主从快照边界。现在四个索引共享变更和返回契约。清空先分配空索引，失败保留旧数据。RBTree 销毁改为一次后序遍历，Hash 将字节求和改为 FNV-1a。 | `src/engine/kvs_engine.c`、`src/store/kvs_data.c`、`src/store/kvs_{hash,rbtree}.c` |
| AOF | 原先忽略追加失败，且清空在线索引后直接回放。现在启动自动恢复，写成功先完整追加再回复和发布。短写尝试回滚，追加失败停止进程，故障写不返回成功。仅截断可判定为不完整的最终记录，回放失败保留在线索引。 | `src/store/kvs_incremental.c` |
| Snapshot | 固定临时名存在覆盖风险，旧路径也缺目录同步。现在写同目录唯一临时文件，先同步文件，再原子替换并同步目录。加载校验格式、边界、CRC 和字符串字段后再安装。 | `src/store/kvs_file.c`、`src/store/kvs_persistence.c` |
| Replication | 共享端口交接时，复制线程可以先关闭 socket，而网络仍按同一个 fd 清理。现在分别持有描述符。慢从库断开后使用全量同步重建状态。启用 AOF 的从库先持久化新检查点，再安装并确认，临时文件写失败时保留旧 AOF 和在线数据。 | `src/replication/kvs_{master,slave}.c`、`src/store/kvs_persistence.c` |

`full` 模式通过 `SAVE` 和 `LOAD` 显式管理快照。 `aof` 模式启动回放，普通写的确认覆盖 OS 缓存中的完整追加，`SAVE` 才执行 `fsync`。两种模式分别恢复。若从库在替换 AOF 后无法同步目录，进程退出并在下次启动重新恢复。文件 I/O 在现有同步提交边界内使用 `writev` 和 `fsync`，`io_uring` 继续用于 Proactor。Linux 参考 [fsync(2)](https://man7.org/linux/man-pages/man2/fsync.2.html) 与 [liburing 提交语义](https://github.com/axboe/liburing/blob/master/man/io_uring_submit.3)。

## 验证

- GCC 11.4 默认构建、`make test_unit test_allocator`、四索引及文本和 RESP 客户端回归通过。libc 的 ASan／UBSan 构建重复通过。
- Reactor 与 Proactor 分别通过分片、2,048 条流水线请求、1,200 条空闲连接、32 个并发客户端的读写和计数校验、慢读客户端及带在途请求的关闭。
- 通过 AOF 进程强杀重启、尾记录截断、非法记录和不可回放记录、短写失败与保留在线数据；通过快照短写、坏 CRC、截断与旧文件保留。
- TCP 共享与独立复制端口通过增量顺序、慢从库断开、重连和全量覆盖。从库 AOF 通过离线重启及全量检查点写失败。eBPF 在 WSL 内通过独立端口复制端到端测试。
- NtyCo 通过网络用例。三种分配器均通过各自的分配器测试。RDMA、eBPF、MySQL 可一起编译，`PROACTOR=0 ALLOCATOR=libc` 构建无需链接 liburing。

复现入口为 [tests/test_reliability.py](tests/test_reliability.py)、[tests/test_sync_e2e.py](tests/test_sync_e2e.py) 和 [tests/benchmark.py](tests/benchmark.py)。

## Redis Benchmark

WSL2 Ubuntu 22.04，内核 6.18.54.1，Core Ultra 7 265KF，GCC 11.4 `-O2`，redis-benchmark 6.0.16。服务端和客户端分别绑 CPU 2 和 4，TCP loopback。Hash 预置 8,192 个键，64 B 值，64 条长连接，pipeline 1。每组分别跑 100% SET 更新和 100% GET 命中，每项先预热 10,000 次，再测 50,000 次，三轮交替次序并取中位数。关闭持久化、复制及审计，RSS 为一轮结束后的进程采样。[逐轮数据](docs/benchmarks/refactor-2026-11-10.csv) · [Redis Benchmark 文档](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/benchmarks/)

| 配置 | SET QPS | SET P50/P99 ms | GET QPS | GET P50/P99 ms | RSS KiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| 基线 Reactor / jemalloc | 143,266 | 0.364 / 0.929 | 170,648 | 0.299 / 0.646 | 5,444 |
| 改后 Reactor / jemalloc | 153,374 | 0.317 / 0.936 | 176,678 | 0.278 / 0.643 | 5,328 |
| 基线 Proactor / jemalloc | 117,647 | 0.414 / 0.948 | 141,643 | 0.340 / 0.688 | 5,520 |
| 改后 Proactor / jemalloc | 149,701 | 0.336 / 0.910 | 169,492 | 0.294 / 0.644 | 5,452 |
| 改后 Reactor / libc | 153,846 | 0.315 / 0.961 | 172,414 | 0.286 / 0.649 | 3,244 |
| 改后 Reactor / mmpool | 150,150 | 0.323 / 0.904 | 177,936 | 0.276 / 0.626 | 4,900 |

该负载下，Reactor 的 SET / GET QPS 中位数增加 7.1% / 3.5%，Proactor 增加 27.2% / 19.7%。Reactor SET 的 P99 从 0.929 ms 到 0.936 ms。三轮内各分配器排序随指标变化，保留默认 jemalloc。

另用链接计数测量独立的 Reactor 负载，含预装 8,192 键及 8,192 组 SET + GET。应用层 `kvs_malloc/calloc/realloc` 调用总数由 51,212 降到 32,779（−36.0%）。单从库跟随时，主进程的互斥锁调用为 jemalloc 24,665 次、自研池 155,802 次，其中获取耗时 ≥10 µs 分别为 0 和 3 次。计时包含调度影响，只覆盖项目显式调用。当前小数据集未观察到持续锁等待。[计数原始结果](docs/benchmarks/refactor-allocation-lock-2026-11-10.txt)

## 剩余边界

- 普通 AOF 确认和每次写入落盘之间仍有窗口。故障验证覆盖进程强杀、文件长度限制和损坏注入，物理掉电、存储设备回写故障和裸机性能待验证。AOF 沿用 RESP 格式，完整记录的静默位翻转仍需要校验机制才能识别。
- 多键命令发生中途资源故障时仍可能提交前缀。快照加载需要两套索引的峰值内存，手动 `LOAD` 和同步文件操作可能阻塞服务。文本要求行结束符，单条请求上限为 64 KiB，RESP 保留单条命令的 1 KiB 响应缓冲区和字符串存储模型。mmap 恢复要求文件由实例管理，外部进程不得在读取时截断文件。
- 复制为异步单主，断线期间从库可能提供旧数据。现有连接内序号用来检测不连续，重连通过全量重同步。跨重连 backlog、增量续传与自动故障切换暂未实现。
- NtyCo 仍依赖第三方协程库的进程信号退出。RDMA 缺少真实设备，MySQL 缺少服务端实例，本轮仅验证相关构建。固定桶数 Hash 和自研池的规模上限需要更大数据量与多线程负载再测。
