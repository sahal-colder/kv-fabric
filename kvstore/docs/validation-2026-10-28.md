# 配置与复制改造验证记录

日期：2026-10-28。环境：Ubuntu 22.04 / WSL2，Linux 6.18.54.1-microsoft-standard-WSL2，GCC、Clang。

已安装开发依赖：`clang`、`libbpf-dev`、`librdmacm-dev`、`libibverbs-dev`、`pkg-config`，以及安装时引入的 libelf/LLVM 等依赖。系统原有 liburing、jemalloc、MySQL 开发包保留。

## 已完成

| 检查 | 结果 |
| --- | --- |
| 默认 jemalloc、无 RDMA/BPF/MySQL/NtyCo 构建 | 通过；输出在 `build/check-default/` |
| 默认构建的配置、复制帧、角色和快照单元测试 | 通过 |
| libc + RDMA + BPF 构建及 BPF 内核目标编译 | 通过；输出在 `build/` |
| RDMA + BPF + MySQL + NtyCo 全可选模块构建 | 通过；输出在 `build/check-optional/` |
| 未编译 RDMA/BPF 时显式请求对应能力 | 正确拒绝 |
| 五份配置样例 `--check-config` | 通过；没有启动 RDMA 连接 |
| 配置解析、CLI 覆盖、错误原子性、密码隐藏 | 通过 |
| 64 位序号、二进制帧、CRC 损坏、截断及长度上限 | 通过 |
| 现有 array/rbtree/hash/skiptable/protocol/redis 六组客户端 | 通过 |
| 运行时 full 快照、AOF、Proactor 选择与关闭 | 通过 |
| 主节点 LOAD 后强制重新全量同步 | 通过 |
| TCP 复用端口和独立端口的主从同步 | 通过 |
| 大于 1 KiB 的初始快照、各引擎增量、DEL/FLUSH、只读从节点 | 通过 |
| 心跳、主节点重启、超过三次失败后继续重连 | 通过 |
| 暂停副本期间 15000 次写入，恢复后追平最终值 | 通过 |
| eBPF 内核验证器、独立临时 bpffs 下的同机复制及暂停恢复 | 通过 |
| 新增 Python 文件 Ruff 格式/静态检查和语法编译 | 通过 |

测试运行的进程会退出；eBPF 测试的 pinned map 与临时 bpffs 在结束时清理。本次不提供吞吐提升百分比，也未进行设备性能测量。原有存储代码及第三方代码仍有历史编译警告；本次验证不代表整个仓库已达到无警告状态。

## 明确暂缓

- **RDMA 真机端到端测试**：当前 WSL 没有 `/sys/class/infiniband` 设备。已完成真实 rdma-core API 编译和链接，没有用 TCP 仿真冒充 RDMA 传输结果。
- **MiniInfer / vLLM 联调和性能测试**：按用户要求，等待 MiniInfer 功能完善后再进行。仅准备了独立目录客户端、配置与接口契约，没有启动实际推理或修改 MiniInfer 项目。
- **MySQL 服务写入测试**：已验证可选构建，未启动或修改本机数据库。
- **NtyCo 运行时回归**：已验证可选构建，默认运行和关闭验证使用 Reactor / Proactor。

## 重现

```bash
make -j4 ALLOCATOR=libc RDMA=1 BPF=1 all bpf
make ALLOCATOR=libc RDMA=1 BPF=1 test_unit
python3 tests/test_regressions.py
python3 tests/test_sync_e2e.py
sudo python3 tests/test_sync_e2e.py --ebpf
```

MiniInfer 接入契约、RDMA/eBPF 部署方式、能力边界和备份 SHA256 见 [配置与集成说明](config-sync-miniinfer.md)。
