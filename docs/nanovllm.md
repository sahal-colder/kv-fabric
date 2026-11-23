# nano-vLLM C

独立的 C 推理引擎，包含分词、调度、分页 KV、前缀复用、采样和模型加载。
支持 legacy FP32 Llama 权重。从仓库根目录构建：

```bash
cmake -S . -B build-nano -DCMAKE_BUILD_TYPE=Release \
  -DMINIINFER_WITH_KVFABRIC=OFF -DMINIINFER_BUILD_STORAGE=OFF -DMINIINFER_BUILD_INTEGRATION=OFF
cmake --build build-nano -j
./build-nano/bin/nanovllm /path/to/stories15M.bin -z /path/to/tokenizer.bin
```

用 `-DNANO_WITH_CUDA=ON` 构建 CUDA 版，再传 `--device cuda`。这个产品
自带模型映射和默认缓冲区分配。`include/nanovllm/nv.h` 提供直接调用的引擎和
KV 缓存 API。仓库根目录的 `miniinfer` 目标接入 MiniInfer Storage。

## 跨 Worker 前缀复用

使用默认 CMake 构建同时启用 KV-Fabric 和 MiniInfer Storage，即可为 `nanovllm` 与 `miniinfer` 传入 `--remote-kv`。`MODEL` 与 `-z PATH` 分别提供检查点和词表，Worker 从实际加载的数据生成身份摘要。

| 选项 | 含义 |
| --- | --- |
| `--remote-kv` | 启用后在本地前缀未覆盖完整页时尝试远程预取。默认关闭 |
| `--stdin` | 每行处理一个提示词并保留缓存，读到 EOF 后退出。与 `-i` 二选一 |
| `--kv-control-endpoint IP:PORT` | 目录地址，默认 `127.0.0.1:9000` |
| `--kv-listen-endpoint IP:PORT` | 本地 Worker 监听地址，默认 `127.0.0.1:0` |
| `--kv-advertise-endpoint IP:PORT` | 其他 Worker 可访问的地址，省略时使用绑定地址 |
| `--kv-tenant NAME` | 命名空间租户，默认 `default` |
| `--kv-page-size N` | 每页 Token 数。与 `--block-size` 同时提供时须相等 |
| `--kv-cache-capacity N` | Worker 的导出页容量，缺省取 `--blocks` |
| `--kv-prefetch-timeout MS` | 预取时间上限，默认 `1000` 毫秒，支持 `1` 到 `30000` |
| `--kv-worker-name NAME` | 固定注册名，适合在重启时替换旧进程注册 |
| `--kv-tp-size N`、`--kv-pp-size N` | 两项均要求为 `1` |

远程 KV 需要本地前缀索引。使用 `-i` 时，CLI 在最后一个请求结束后注销 Worker。使用 `--stdin` 时，CLI 在每条请求后输出本批次前向计数和 Worker 累计远程指标，保留模型、已缓存页和 Worker 注册直到 EOF。在 Linux 交互终端用 `Ctrl+D` 结束输入。

嵌入式服务可让同一个 `NvEngine` 持续接收请求。启用远程 KV 后，用 `nv_engine_has_work()` 判断是否还有在途请求。`nv_engine_step()` 等待远程页时可以返回零。读取完请求结果后，可在引擎空闲时调用 `nv_engine_clear_requests()` 释放结果和回收请求 ID，保留 KV 缓存。

进程验收命令、布局、状态机和指标定义见 [Nano-VLLM 远程 KV 集成与验收](NANOVLLM-REMOTE-KV-INTEGRATION.md)。
