# MiniInfer Storage

独立的 C 存储运行库，提供主机内存池、可选 CUDA 显存池、CPU/GPU/SSD
分层存储和 KV 卸载。公开头文件在 `include/miniinfer`。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/miniinfer_basic
ctest --test-dir build --output-on-failure
```

`MINIINFER_WITH_URING` 和 `MINIINFER_WITH_SPDK` 独立控制 SSD 后端。
SPDK 通过 `INFER_SPDK_ROOT` 指定 SDK。用 `MINIINFER_WITH_CUDA=ON` 构建
显存池时需要 CUDA Toolkit 的运行库和头文件。
