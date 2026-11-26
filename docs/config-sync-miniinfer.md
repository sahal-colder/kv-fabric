# 配置、复制传输与 MiniInfer 对接

## 构建和启动

运行环境为 Linux / WSL2。普通构建默认不依赖 RDMA、libbpf、MySQL 和 NtyCo；liburing、pthread 与 OpenSSL libcrypto 仍然需要；默认分配器为 libc。

```bash
sudo apt-get install build-essential liburing-dev libssl-dev
make -j4
./build/bin/kvstore --config config/kvstore.conf

# 不启动网络、不分配引擎、不连接 MySQL，只检查配置。
./build/bin/kvstore --config config/kvstore.conf --check-config
./build/bin/kvstore --config config/kvstore.conf --print-config
```

位置参数接口保留，但 master/slave 必须另配 `replication-secret`，master 必须提供独立 `replication-port`；缺失时安全拒绝启动。新接口允许省略位置参数，默认端口为 2000。每个进程只监听配置的客户端端口，不再额外打开后续 19 个端口。默认绑定 127.0.0.1；跨主机部署时显式设置 `bind`。

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
| `replication-secret` | 无默认值；master/slave 必填，相同的私有随机 32..128 字节密钥；打印脱敏 |
| `replication-port` | master 必须设置非零独立端口，TCP/RDMA 均不再复用客户端监听器 |
| `sync-timeout-ms` | `5000`，至少 `2000`；一次完整头部/载荷传输的期限，防止碎片慢流量无限续期 |
| `sync-reconnect-ms` | `1000`，重连间隔；直到停止进程持续重试 |
| `sync-queue-bytes` | `8388608`；每副本等待发送的增量队列上限，不含正在发送的一帧 |
| `sync-snapshot-bytes` | `67108864`；每次生成/接收全量快照的上限 |
| `ebpf-enabled` | `no`；仅可与 TCP 组合 |
| `ebpf-object` | `build/bpf/kvs_sockhash.bpf.o` |
| `ebpf-map` | `/sys/fs/bpf/kvstore_sync`；参与同机加速的进程必须共享此路径 |

MySQL 和持久化已通过运行时配置选择；分配器、可选网络库、RDMA 和 eBPF 通过构建选项选择。显式请求未编译能力会失败，不会继续使用其他模式。

## TCP / RDMA 共用的复制语义

复制使用 `KVS_SYNC_AUTH_V2` 握手，在生成快照或占用副本槽位之前，以双随机 nonce 与带角色标签的 HMAC-SHA256 验证双方。旧 V1 不降级接受。密钥可用 `openssl rand -hex 32` 生成并写入权限受限的双方配置文件；示例不提供公共默认密钥。握手最多 1 秒，使用同一绝对期限；HMAC 不加密后续数据。每帧包含 24 字节网络字节序头部：magic、类型、载荷长度、CRC32、64 位序号。帧类型为快照、增量、心跳和确认。

1. 主节点持有存储读锁生成有大小上限的快照，在同一锁区间登记副本及序号屏障。
2. 随后释放存储锁，由副本专用线程发送快照；期间的新写入进入有界增量队列。网络 I/O 不持有存储锁。
3. 从节点校验长度、CRC 和完整快照格式，在临时引擎中构建数据，成功后原子替换现有引擎。确认后才报告链路 `up`。
4. 增量必须严格连续；内部命令保留所属引擎，因此传统 `HSET` / `RSET` 等写入也能正确复制。
5. 空闲和持续写入期间都会发送心跳，确认副本已经应用之前的数据。超时、序号缺口、校验错误或队列超限会断开连接；重连重新执行全量同步，清除旧主节点遗留的多余键。

一次最多管理 64 个副本。每个副本拥有一个发送线程、一个受限快照和一个有界增量队列；应按副本数量规划主机内存。快照捕获仍会暂时阻塞写入，并非无停顿快照；此实现面向当前项目规模。

这是异步单主复制：客户端成功响应不表示副本已确认，不提供 quorum、自动选主或故障切换一致性保证。旧版复制协议不兼容，主从应一起升级。从节点使用 `persistence aof` 时，全量同步先验证并构建临时索引，再原子替换 AOF，安装数据后才确认同步。AOF 启动时自动重放，副本在重新连上主节点前可能提供上次恢复的旧数据。`full` 模式保留进程内部快照 API；远程文本/RESP `SAVE`、`LOAD` 一律拒绝，避免未认证文件管理。

TCP 示例（先在两个私有配置文件中填入相同的 `replication-secret`）：

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
