# MiniInfer Storage

独立的 C 存储运行库，提供主机内存池、可选 CUDA 显存池、CPU/GPU/SSD
分层存储和 KV 卸载。公开头文件在 `include/miniinfer`。从仓库根目录构建本地存储库。

```bash
cmake -S . -B build-storage -DCMAKE_BUILD_TYPE=Debug \
  -DMINIINFER_WITH_KVFABRIC=OFF -DMINIINFER_BUILD_NANOVLLM=OFF -DMINIINFER_BUILD_INTEGRATION=OFF
cmake --build build-storage -j
./build-storage/bin/miniinfer_basic
ctest --test-dir build-storage --output-on-failure
```

`MINIINFER_WITH_URING` 和 `MINIINFER_WITH_SPDK` 独立控制 SSD 后端。
SPDK 通过 `INFER_SPDK_ROOT` 指定 SDK。用 `MINIINFER_WITH_CUDA=ON` 构建
显存池时需要 CUDA Toolkit 的运行库和头文件。
