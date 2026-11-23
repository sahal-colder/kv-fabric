# KV-Fabric

KV-Fabric 是一套 C11 分布式 KV Cache。目录服务维护 KV 前缀、副本和租约。Worker 通过 TCP 直接传输 Tensor，使用 CPU 或 NVIDIA CUDA 导入本地分层缓存。

| 路径 | 职责 |
| --- | --- |
| [src/control](src/control/) | Wire V3、全局目录、心跳、租约与 C SDK |
| [src/worker](src/worker/) | 设备传输、Segment 池、连接池、前缀索引和分层存储 |
| [include](include/) | `kvfabric` 与 `miniinfer` 的 C API |
| [tests](tests/) | 控制面、Worker 和通用 KVStore 回归 |

## 构建与测试

在 Linux 或 WSL 上安装 C11 编译器、CMake 3.18+ 和 OpenSSL 开发包。从仓库根目录一次构建目录服务、CPU Worker 和相关工具。

```sh
git clone https://github.com/sahal-colder/kv-fabric.git
cd kv-fabric
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
ctest --test-dir build-release --output-on-failure
cmake --install build-release --prefix "$PWD/build-release/install"
```

运行 `build-release/bin/kvfabricd --config config/kvfabric.conf` 启动目录服务。默认监听 `127.0.0.1:9000`。同机和两台 Linux 机器上的 Worker 运行命令见部署文档。

| 可选构建开关 | 用途 |
| --- | --- |
| `-DMINIINFER_WITH_CUDA=ON` | 启用 Worker 的 CUDA C Runtime 搬运，需要 CUDA Toolkit |
| `-DMINIINFER_WITH_URING=ON` | 启用 io_uring 文件后端，需要 liburing 开发包 |
| `-DKVF_SANITIZE=ON` | 启用 ASan 和 UBSan，Linux 可同时检查泄漏 |
| `-DMINIINFER_WITH_KVFABRIC=OFF` | 构建单机存储或推理组件 |

## 文档

- [CUDA、io_uring 和双机演示](docs/kvfabric-data-plane.md)
- [从 0 阅读代码指南](docs/READING-GUIDE.md)
- [实现与实测报告](docs/KV-FABRIC-IMPLEMENTATION-REPORT.md)
- [通用 KVStore 的 Make 构建与测试](docs/kvstore.md)
- [MiniInfer 存储库与推理 CLI](docs/miniinfer.md)
