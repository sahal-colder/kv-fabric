# KV-Fabric

KV-Fabric 是一套面向推理工作节点的 C11 分布式 KV Cache。控制面维护前缀目录、副本和租约。Worker 之间通过 TCP 传输 KV Tensor，并使用 CPU 或 NVIDIA CUDA 导入本地缓存。

| 模块 | 职责 |
| --- | --- |
| [KVStore 控制面](kvstore/README.md) | `kvfabricd`、二进制 RPC、全局目录、心跳和租约 |
| [MiniInfer-C 数据面](miniinfer-c/README.md) | Worker、连接池、逐层传输、Prefix Trie 与分层存储 |

## Linux CPU 构建与测试

安装 C11 编译器、CMake 3.18+ 和 OpenSSL 开发包。

```sh
git clone --branch dev https://github.com/sahal-colder/kv-fabric.git
cd kv-fabric
scratch=$(mktemp -d /tmp/kvfabric-build.XXXXXX)

cmake -S kvstore/kvfabric -B "$scratch/control" -DCMAKE_BUILD_TYPE=Release
cmake --build "$scratch/control" -j
ctest --test-dir "$scratch/control" --output-on-failure
cmake --install "$scratch/control" --prefix "$scratch/sdk"

cmake -S miniinfer-c/products/miniinfer-storage -B "$scratch/worker" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$scratch/sdk" \
  -DMINIINFER_WITH_KVFABRIC=ON
cmake --build "$scratch/worker" -j
ctest --test-dir "$scratch/worker" --output-on-failure
```

- [CUDA、io_uring 与双机演示](miniinfer-c/docs/kvfabric-data-plane.md)
- [从 0 阅读代码指南](miniinfer-c/docs/READING-GUIDE.md)
- [实现报告、回归与实测结果](miniinfer-c/docs/KV-FABRIC-IMPLEMENTATION-REPORT.md)
