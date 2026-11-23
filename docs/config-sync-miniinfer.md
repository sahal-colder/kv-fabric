# 配置、复制传输与 MiniInfer 对接

## 构建和启动

运行环境为 Linux / WSL2。普通构建默认不依赖 RDMA、libbpf、MySQL 和 NtyCo；liburing、pthread 和默认分配器 jemalloc 仍然需要。

```bash
sudo apt-get install build-essential liburing-dev libjemalloc-dev
make -j4
./build/bin/kvstore --config config/kvstore.conf

# 不启动网络、不分配引擎、不连接 MySQL，只检查配置。
./build/bin/kvstore --config config/kvstore.conf --check-config
./build/bin/kvstore --config config/kvstore.conf --print-config
```

旧式 `kvstore 2000 hash --role master` 仍可使用。新接口允许省略位置参数，默认端口为 2000。每个进程只监听配置的客户端端口，不再额外打开后续 19 个端口。默认绑定 127.0.0.1；跨主机部署时显式设置 `bind`。

配置格式为 `directive value`，支持空行、`#` 注释、单双引号和 `\\`、`\"`、`\'`、`\n`、`\r`、`\t` 转义。未知指令、额外参数、越界数字和不兼容组合均会报错。配置读取失败不会部分修改原配置。优先级为：默认值 → 配置文件 → 命令行；CLI 覆盖与 `--config` 的位置无关。路径相对于进程工作目录。配置仅在启动时应用，未实现热更新。

所有配置项都可以写为 `--directive value`。`--print-config` 输出主要生效设置，密码始终隐藏；该输出供检查使用，不是完整配置文件导出。

| 配置项 | 默认值 / 语义 |
| --- | --- |
| `bind`, `port` | `127.0.0.1`, `2000`；IPv4 监听地址 |
| `engine` | `array`；可选 `rbtree` / `hash` / `skiplist` |
| `network` | `reactor`；`proactor` 需要 `PROACTOR=1`，`ntyco` 需要 `NTYCO=1` |
| `persistence` | `full`；可选 `aof` / `none`，编译期宏只提供默认值 |
| `snapshot-file`, `aof-file` | `data/kvstore.snapshot`, `data/kvstore.aof` |
| `mysql-enabled` | `no`；启用前须以 `MYSQL=1` 编译 |
| `mysql-host`, `mysql-port` | `127.0.0.1`, `3306` |
| `mysql-user`, `mysql-password` | `root`，空密码；配置文件可用引号包含特殊字符 |
| `mysql-database`, `mysql-table` | `kvstore`, `kvs_records` |
| `role` | `standalone` / `master` / `slave` |
| `master-host`, `master-port` | 从节点必须提供；端口是主节点实际的复制端点 |
| `sync-transport` | `tcp` / `rdma`，显式选择，不自动降级 |
| `replication-port` | `0`：TCP 复用客户端监听器；非零：独立复制监听器；RDMA 主节点必须使用独立端口 |
| `sync-timeout-ms` | `5000`，至少 `2000`；一次完整头部/载荷传输的期限，防止碎片慢流量无限续期 |
| `sync-reconnect-ms` | `1000`，重连间隔；直到停止进程持续重试 |
| `sync-queue-bytes` | `8388608`；每副本等待发送的增量队列上限，不含正在发送的一帧 |
| `sync-snapshot-bytes` | `67108864`；每次生成/接收全量快照的上限 |
| `ebpf-enabled` | `no`；仅可与 TCP 组合 |
| `ebpf-object` | `build/bpf/kvs_sockhash.bpf.o` |
| `ebpf-map` | `/sys/fs/bpf/kvstore_sync`；参与同机加速的进程必须共享此路径 |

MySQL 和持久化已通过运行时配置选择；分配器、可选网络库、RDMA 和 eBPF 通过构建选项选择。显式请求未编译能力会失败，不会继续使用其他模式。

## TCP / RDMA 共用的复制语义

复制使用版本化握手 `KVS_SYNC_HELLO_V1` / `KVS_SYNC_OK_V1`。每帧包含 24 字节网络字节序头部：magic、类型、载荷长度、CRC32、64 位序号。帧类型为快照、增量、心跳和确认。

1. 主节点持有存储读锁生成有大小上限的快照，在同一锁区间登记副本及序号屏障。
2. 随后释放存储锁，由副本专用线程发送快照；期间的新写入进入有界增量队列。网络 I/O 不持有存储锁。
3. 从节点校验长度、CRC 和完整快照格式，在临时引擎中构建数据，成功后原子替换现有引擎。确认后才报告链路 `up`。
4. 增量必须严格连续；内部命令保留所属引擎，因此传统 `HSET` / `RSET` 等写入也能正确复制。
5. 空闲和持续写入期间都会发送心跳，确认副本已经应用之前的数据。超时、序号缺口、校验错误或队列超限会断开连接；重连重新执行全量同步，清除旧主节点遗留的多余键。

一次最多管理 64 个副本。每个副本拥有一个发送线程、一个受限快照和一个有界增量队列；应按副本数量规划主机内存。快照捕获仍会暂时阻塞写入，并非无停顿快照；此实现面向当前项目规模。

这是异步单主复制：客户端成功响应不表示副本已确认，不提供 quorum、自动选主或故障切换一致性保证。旧版复制协议不兼容，主从应一起升级。从节点使用 `persistence aof` 时，全量同步先验证并构建临时索引，再原子替换 AOF，安装数据后才确认同步。AOF 启动时自动重放，副本在重新连上主节点前可能提供上次恢复的旧数据。`full` 模式仍通过 `SAVE` 和 `LOAD` 显式管理本地快照。

TCP 示例：

```bash
./build/bin/kvstore --config config/miniinfer-master.conf
./build/bin/kvstore --config config/miniinfer-slave.conf
```

RDMA 使用 rdma-core 的 **rsockets** API，真实调用 `rsocket/rconnect/raccept/rsend/rrecv/rpoll`。它管理 QP、注册内存和 RDMA 连接，是 RDMA 可靠字节流接口，不是单边 RDMA WRITE，也不声称端到端零拷贝。两端都必须选择 RDMA，且具备可用的 RDMA 设备和地址路由。

```bash
sudo apt-get install librdmacm-dev libibverbs-dev
make -j4 RDMA=1
# 编辑示例中的 192.0.2.x，占位地址不能直接使用。
./build/bin/kvstore --config config/rdma-master.conf
./build/bin/kvstore --config config/rdma-slave.conf
```

## eBPF 同机 TCP 加速

用户态只将复制连接登记到共享 `SOCKHASH`。`SK_MSG` 程序根据反向 IPv4 四元组查找同机接收端，并通过 `bpf_msg_redirect_hash(..., BPF_F_INGRESS)` 将数据送到对端接收队列。主机上没有对端映射时继续使用普通 TCP。RDMA 数据绕过这条 socket 路径，因此配置不允许同时对 RDMA 使用该 eBPF 加速。

程序不处理 RESP、写入顺序或一致性；这些仍由复制层负责。只有同机双方共享 pinned map 时才可能走重定向路径；跨机器不会获得这种本地捷径。实际收益需要在目标工作负载上测量。

```bash
sudo apt-get install clang libbpf-dev libelf-dev zlib1g-dev
make -j4 RDMA=1 BPF=1 bpf all
sudo mount -t bpf bpf /sys/fs/bpf  # 已挂载时跳过
sudo ./build/bin/kvstore --config config/miniinfer-master.conf --ebpf-enabled yes
sudo ./build/bin/kvstore --config config/miniinfer-slave.conf --ebpf-enabled yes
```

需要内核支持 SOCKHASH / SK_MSG，以及相应 BPF 权限。初始化失败会终止启动。连接结束时删除对应键；pinned map 保留给其他进程和后续启动复用。全部参与进程停止后，可按配置路径显式删除 pinned map 以卸载它；不要删除仍被其他进程使用的 map。替换 BPF 程序后应在维护窗口清理旧 map 再启动，以免复用旧程序。

## MiniInfer 对接准备：联调待执行

`integrations/miniinfer/kvstore_directory.py` 是独立、可选的元数据客户端，未改动 MiniInfer 的算子、调度器或分层存储实现。它使用 Python 标准库，按 identity 隔离模型/布局，并以两条一组的 MGET 查询连续前缀。单条 manifest 限制为 320 字节，避免触及当前 RESP 响应容量；不通过现有字符串引擎保存张量。

```python
from kvstore_directory import BlockRef, Directory
import time

directory = Directory({
    "model_revision": "exact-weight-revision",
    "layout": {"block_tokens": 16, "kv_heads": 8, "head_dim": 128},
    "dtype": "float16",
    "tp_rank": 0,
    "tp_size": 1,
    "adapter_revision": "none",
    "cache_salt": "tenant-a",
})
# block_hash 必须来自包含前序上下文的哈希链。
# 完成数据写入及 CUDA Event 后才能 publish。
directory.publish(block_hash, BlockRef(
    owner="worker-a:boot-uuid", endpoint="unix:/run/miniinfer/worker-a.sock",
    slot=42, generation=7, nbytes=1048576,
    expires_unix_ms=time.time_ns() // 1_000_000 + 60_000,
))
references = directory.lookup_prefix(block_hashes)
directory.close()
```

引用中的 endpoint 是将来由 MiniInfer 提供的载荷服务端点，本次没有伪造或启用该服务。当前 MiniInfer 的私有 SSD 文件不能通过登记路径就变成共享文件。目录项只是候选位置，**不能将查询结果直接当作有效 GPU KV**：MiniInfer 必须在读取时获取自己的 lease，并同时检查 owner 启动标识、内容身份、槽位 generation、长度与校验和。过期检查依赖客户端墙钟，仅用于减少无效命中，不替代服务端租约。元数据可被更新，当前单项仅保存一个位置；读取失败应回退到重新计算。

MiniInfer 功能完善后，再显式执行以下联调：

1. 接入 Connector 的前缀匹配、payload 完成回调和失效路径，确认末 token 的调度契约。
2. A 实例发布完整块，B 实例恢复；覆盖不同模型、layout、dtype、TP rank、LoRA 和租户隔离。
3. 覆盖槽位复用、owner 重启、过期、CRC 错误、目录不可达和主从重连；失败均回退重算。
4. 比较原生 vLLM、本地 MiniInfer、接入目录三组：输出 token 正确性、实际复用 token、TTFT P50/P95、复制滞后、CPU/SSD/网络字节数。
5. 在有 RDMA 设备的目标机器上验证连接失败、恢复、长时间传输及吞吐；eBPF 与普通 TCP 使用相同请求轨迹分别测量。

本阶段不运行真实 MiniInfer / vLLM 联调或推理性能测试。

## 独立验证

```bash
make ALLOCATOR=libc RDMA=1 BPF=1 test_unit
python3 tests/kvstore/test_sync_e2e.py
python3 tests/kvstore/test_regressions.py
sudo python3 tests/kvstore/test_sync_e2e.py --ebpf
```

eBPF 测试挂载临时、隔离的 bpffs，并在退出时清理 map 和挂载；不会覆盖运行中部署的 pinned map。配置与协议单元测试覆盖错误配置的原子性、CLI 覆盖、密码隐藏、64 位序号、含 NUL 的帧、CRC 损坏、超长和截断帧。端到端测试覆盖大于 1 KiB 的全量快照、各引擎增量、只读从节点、心跳和主节点重启后的全量恢复。

独立复制端口的测试还会暂停从节点，并持续执行 15000 次写入，随后恢复副本并检查最终内容。回归入口覆盖原有六组客户端、运行时快照/AOF 选择、LOAD 后的复制重建，以及 Proactor 启动与正常关闭。默认 Reactor 和 Proactor 支持本次加入的有序关闭；第三方 NtyCo 的 accept hook 不提供取消接口，继续使用进程信号退出，不承诺排空任务。

## 参考实现

实现按当前项目接口编写，参考以下成熟项目的设计和公开 API，没有复制其整段源码：

- [Redis 8.2 配置示例](https://github.com/redis/redis/blob/8.2/redis.conf)：文本指令、默认值与命令行覆盖。
- [linux-rdma/rdma-core rsockets 文档](https://github.com/linux-rdma/rdma-core/blob/master/librdmacm/man/rsocket.7.in)：可靠 RDMA socket API 和 poll 接口。
- [Linux SOCKMAP/SOCKHASH 自测](https://github.com/torvalds/linux/blob/master/tools/testing/selftests/bpf/progs/test_sockmap_kern.h)：SK_MSG 程序、socket map 与 ingress 重定向。
- [Linux 内核 SOCKMAP 文档](https://docs.kernel.org/bpf/map_sockmap.html)：map/程序生命周期与重定向语义。

## 改动前备份

完整备份位于项目目录外，包含当时的 `.git`、未提交修改及未跟踪文件：

`C:\Users\wtyjt\Desktop\kvstore-backups\kvstore-before-config-sync-20261028-164400.tar.gz`

SHA256：`2135BF2170EC67F540588F2ADAAA791DB921955E2152CF9199E0CBF501905FEC`

恢复时先解压到新目录，核对后再使用，避免覆盖之后产生的工作。没有回滚用户原有的分配器、Makefile 和测试修改。
