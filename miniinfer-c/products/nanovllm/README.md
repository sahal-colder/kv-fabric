# nano-vLLM C

独立的 C 推理引擎，包含分词、调度、分页 KV、前缀复用、采样和模型加载。
支持 legacy FP32 Llama 权重。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/nanovllm /path/to/stories15M.bin -z /path/to/tokenizer.bin
```

用 `-DNANO_WITH_CUDA=ON` 构建 CUDA 版，再传 `--device cuda`。这个产品
自带模型映射和默认缓冲区分配。`include/nv.h` 提供直接调用的引擎和
KV 缓存 API。仓库根目录的 `miniinfer` 目标接入 MiniInfer Storage。
