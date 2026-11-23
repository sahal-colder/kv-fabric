# MiniInfer C 集成验证与性能

## 同模型对照

环境为 WSL2，Intel Core Ultra 7 265KF，GeForce RTX 5060 Ti，CUDA 12.8。
基线从本机 Infer 仓库 `c7652df` 重新构建，两个项目均用 Release。
模型为 `stories15M.bin`，提示词为 `Once upon a time`，greedy 生成 64 个
token，SSD 不参与。每组预热一次，交替运行 5 次，关闭 BLAS/OpenMP
的多线程并固定在逻辑 CPU 4。所有生成文本逐字节一致。

| 版本 | CPU 生成阶段 token/s | CUDA 生成阶段 token/s | CPU 进程耗时 | CUDA 进程耗时 |
|---|---:|---:|---:|---:|
| 集成版，MiniInfer 主机池和显存池 | 173.21 | 1676.03 | 0.39 s | 0.33 s |
| 集成版，nano CPU KV 数组和 CUDA 直接分配 | 175.65 | 1699.64 | 0.38 s | 0.34 s |
| Infer `c7652df` | 148.62 | 570.28 | 0.49 s | 0.46 s |

全部为 5 次运行的中位数。集成版内部计时从调度开始，Infer 内部计时
还包括提示词编码。进程耗时由同一个 `/usr/bin/time` 测量，包含启动、
模型加载、CUDA 初始化、生成和退出。表中的差距包含执行路径的差异，
不能归因于更换内存池。该数据只覆盖这个小型 FP32 模型和单请求，
不能外推到大模型、并发吞吐或 SSD I/O 性能。

```bash
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1
MODEL=/path/to/stories15M.bin
TOKENIZER=/path/to/tokenizer.bin
REF=/path/to/Infer/build/mini_infer_main
DEVICE=cuda  # 改成 cpu 测 CPU

/usr/bin/time -f %e taskset -c 4 ./build-full/miniinfer \
  "$MODEL" -z "$TOKENIZER" --device "$DEVICE" -i 'Once upon a time' -n 64 -t 0
/usr/bin/time -f %e taskset -c 4 "$REF" \
  --model "$MODEL" --tokenizer "$TOKENIZER" --device "$DEVICE" \
  --prompt 'Once upon a time' --steps 64 --temperature 0 --storage-engine posix
```

## SSD 前缀恢复

将 CPU/GPU 的 KV 池限制为 4 块，每块 8 个 token。按 A、B、A 的顺序运行
三个请求，用 B 把 A 的前缀挤出。POSIX、io_uring 和 SPDK `Malloc0`
均发生 6 次写回与 2 次恢复。CPU 和 CUDA 各测了三个后端，
生成的 token ID 都与关闭 SSD 时的基线一致。

```bash
A='Once upon a time, there was a little girl who loved to play outside every morning.'
B='Yesterday in a distant village, an old man walked to the market for bread and cheese.'
./build-full/miniinfer "$MODEL" -z "$TOKENIZER" --device cuda \
  --blocks 4 --block-size 8 --batch 1 -t 0 -n 8 --ignore-eos --ids \
  --ssd-slots 16 --ssd-mode uring -i "$A" -i "$B" -i "$A"
```

WSL2 只提供虚拟磁盘。SPDK 验证走内存 bdev，尚无物理 NVMe 或 GDS 实测。

## 回归范围

- Debug 版本通过 6 项 CTest 测试。
- CPU Debug、完整 Release、CUDA 直接分配 Release 均通过 nano-vLLM 原有
  CLI 测试。GQA 和 MHA 两个小模型的 CPU/CUDA greedy token ID 一致。
- 在 RTX 5060 Ti 上单独验证了显存池预分配切片、用完扩容、预算拒绝、
  event 完成后复用原地址，以及 `trim` 后重新分配。
- 基准原始日志保存在 WSL 的 `~/.cache/miniinfer-c-unified/bench/`，
  SSD 测试日志在同级 `final-results/`。
