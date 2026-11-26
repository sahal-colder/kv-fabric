# KV-Fabric 工程验证报告

> 历史阶段报告：本页环境与限制只对应当时的WSL2/RTX5060Ti实验。当前原生Linux、RTX4060、完整vLLM和NVMe/RDMA结果见[完整交付指南](PROJECT-DELIVERY.md)。原始大体积归档本地保留；公开脱敏测量见[验证数据](benchmarks/published-validation/README.md)。


## 验证环境

构建和测试使用 Ubuntu 22.04 WSL2、GCC 11.4.0、CMake 3.22.1、OpenBLAS 0.3.20、liburing 2.0、OpenSSL 3.0.2 和 Python 3.10。CUDA Toolkit 为 12.8.93，目标为 SM120，运行于 NVIDIA GeForce RTX 5060 Ti，Windows 驱动 610.88。CMake 的 C 翻译单元命令使用 `-std=c11`。构建、模型和测试溢写文件都在 WSL2 的 ext4 虚拟磁盘上。性能测试使用独立发布进程、消费进程和目录进程，网络为同主机 IPv4 TCP `127.0.0.1`。[正式基准环境记录](benchmarks/published-validation/README.md)

## 干净构建

以下命令在 Linux 环境运行。可以将 `BUILD` 指向任意空目录。测试由各自的 CMake 构建目录运行。

```bash
BUILD=build/ultimate
CUDA_FLAGS=(-DNANO_WITH_CUDA=ON -DMINIINFER_WITH_CUDA=ON
            -DCMAKE_CUDA_COMPILER=/usr/local/cuda-12.8/bin/nvcc
            -DCMAKE_CUDA_ARCHITECTURES=120)
COMMON=(-G Ninja -DMINIINFER_WITH_URING=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1

cmake -S . -B "$BUILD/cpu-release" "${COMMON[@]}" -DCMAKE_BUILD_TYPE=Release
cmake -S . -B "$BUILD/cuda-release" "${COMMON[@]}" -DCMAKE_BUILD_TYPE=Release "${CUDA_FLAGS[@]}"
cmake -S . -B "$BUILD/cpu-asan" "${COMMON[@]}" -DCMAKE_BUILD_TYPE=Debug -DKVF_SANITIZE=ON
cmake -S . -B "$BUILD/cuda-host-asan" "${COMMON[@]}" -DCMAKE_BUILD_TYPE=Debug -DKVF_SANITIZE=ON "${CUDA_FLAGS[@]}"
cmake -S . -B "$BUILD/local-release" "${COMMON[@]}" -DCMAKE_BUILD_TYPE=Release -DMINIINFER_WITH_KVFABRIC=OFF
for config in cpu-release cuda-release cpu-asan cuda-host-asan local-release; do
  cmake --build "$BUILD/$config" -j 8
done

ctest --test-dir "$BUILD/cpu-release" --output-on-failure --parallel 3
ctest --test-dir "$BUILD/cuda-release" --output-on-failure --parallel 3
ctest --test-dir "$BUILD/local-release" --output-on-failure --parallel 3
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir "$BUILD/cpu-asan" --output-on-failure --parallel 3
# WSL2 with this NVIDIA Windows driver. Keep the suppression scoped to the driver library.
printf 'leak:/usr/lib/wsl/lib/libcuda.so.1\n' > "$BUILD/wsl-cuda-driver.lsan"
ASAN_OPTIONS=protect_shadow_gap=0:detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  LSAN_OPTIONS=suppressions="$BUILD/wsl-cuda-driver.lsan" \
  ctest --test-dir "$BUILD/cuda-host-asan" --output-on-failure --parallel 3

cmake -S . -B "$BUILD/tsan" "${COMMON[@]}" -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=thread -fno-omit-frame-pointer -fno-pie' \
  -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build "$BUILD/tsan" -j 6
TSAN_OPTIONS=halt_on_error=1 setarch "$(uname -m)" -R \
  ctest --test-dir "$BUILD/tsan" --output-on-failure --parallel 2
```

| 配置 | 验证项目 |
| --- | --- |
| CPU Release | 14/14 CTest |
| CUDA Release | 17/17 CTest，包含真实 CUDA 推理和存储测试 |
| CPU ASan/UBSan/LSan | 14/14 CTest |
| CUDA Host ASan/UBSan/LSan | 17/17 CTest，使用下述单项 WSL 驱动抑制 |
| CPU TSan | 14/14 CTest，使用进程级 `setarch -R` |
| 关闭 KV-Fabric | 7/7 CTest |

默认 TSan 启动报 `unexpected memory mapping`，使用进程级地址布局设置后跑完测试。CUDA 的 Host ASan 构建也插桩了 CUDA 翻译单元的 Host 部分。默认 ASan 设置下，部分 CUDA 用例在分配阶段失败。独立的 4 KiB `cudaMalloc` 程序复现了同一现象，设置 `protect_shadow_gap=0` 后分配成功。独立程序释放显存后，LSan 仍报告 WSL 驱动 `/usr/lib/wsl/lib/libcuda.so.1` 中的 280 Bytes，CUDA Host 的复测加了该驱动库的单项抑制。CPU LeakSanitizer 没有使用抑制。

### 回归与故障覆盖

- 批量目录测试覆盖连续命中、不同 Worker 的续接、第一个缺口、无效 Boot UUID、旧 Epoch、旧 Fence、Generation/Version 摘要变化和撤销后的 Lease。
- 真实推理集成测试比较生成 Token 和整页 K/V 哈希。覆盖不兼容模型、词表、租户、页大小、根页缺口、最后一个提示 Token、TP/PP 参数、连接截断、帧 CRC、逐层 Manifest CRC、总截止时间、Lease 过期、取消、同名重启、在途 H2D 的取消、缓存打满、导出淘汰、短前缀准入和发布重试。
- 通用 Storage 和 Worker 用例继续覆盖 TLSF、Paged KV、租约、迁移、O_DIRECT、损坏记录、工作线程恢复以及连接池重用和 Boot 切换。
- Kvstore 另用各自干净副本运行 `make -j 8 all test_unit test_allocator`，执行 `test_regressions.py`、`test_sync_e2e.py`、`test_reliability.py`，Release 和 `SANITIZE=1` 均退出 0。独立用 `NTYCO=1` 和 `NTYCO=1 SANITIZE=1` 构建并运行 `KVS_TEST_NETWORK=ntyco python3 tests/kvstore/test_reliability.py`，均退出 0。工作区原有三个 NtyCo 修改参与该验证。

### 真实 CUDA 三轮对话

在同一 RTX 5060 Ti 上，用生产引擎、`stories15M.bin`、8 个生成 Token 和独立 Worker A/B/C 运行以下回归。每一轮比较 Cold 的完整生成 Token ID 和全部完整提示页的 SHA-256。后一轮携带规范化 Token 历史，包含已生成的助手内容和新增用户消息。

```bash
python3 tests/nanovllm/test_remote_inference.py --conversation --device cuda \
  --directory "$BUILD/cuda-release/bin/kvfabricd" \
  --worker "$BUILD/cuda-release/bin/test_nano_remote_worker" \
  --model "$MODEL" --tokenizer "$TOKENIZER"
```

| Worker | 提示 Token | 远程复用 Token | 新发布完整页 | 数据请求 |
| --- | ---: | ---: | ---: | ---: |
| A | 18 | 0 | 6 | 0 |
| B | 32 | 24 | 3 | 1 |
| C | 46 | 36 | 4 | 3 |

### 本地 SSD Spill 与 Remote KV

用生产 `miniinfer --stdin --ids` 依次执行 `The quick brown fox ` 重复四次加 `jumps high`、`A different story begins ` 重复三次加 `in a quiet valley`，再回到首条提示。配置为每页 4 Token、9 个本地页、24 个 SSD 槽、4 个生成 Token、种子 1234、温度 0.7、top-p 0.95 和忽略 EOS。对照使用相同参数并开启 `--no-cache`。分别对 CPU/CUDA、POSIX/io_uring 和启用/关闭 Remote KV 执行，共 8 个组合。每个组合累计 5 页回读、9 页写出，生成 Token ID 与同设备 Cold 对照逐行一致。

## 基准复现

```bash
MODEL=/path/to/stories15M.bin
TOKENIZER=/path/to/tokenizer.bin
python3 scripts/benchmark_nano_remote.py \
  --build "cpu=$BUILD/cpu-release" --build "cuda=$BUILD/cuda-release" \
  --model "$MODEL" --tokenizer "$TOKENIZER" \
  --samples 30 --shard-size 10 --warmup 1 --out "$BUILD/benchmark-warm"
python3 scripts/benchmark_nano_remote.py \
  --build "cuda=$BUILD/cuda-release" --model "$MODEL" --tokenizer "$TOKENIZER" \
  --samples 30 --shard-size 10 --warmup 0 --out "$BUILD/benchmark-matched"
```

脚本验证权重和词表 SHA-256，每 10 轮重启先导阶段的进程。每轮交替 Cold、Local 和 Remote 顺序。每个消费进程独立预热一条不共享目标前缀的请求，Local 组另建目标前缀，Remote 组仅由发布者预热目标前缀。第二组 CUDA 复跑关闭共同预热，便于与原提交同工作负载对照。

正式统计前逐行验证生成一致性、提示长度、5 页复用、276,480 Bytes、目录及数据请求次数、零预取故障和分阶段合计。P50/P95/P99 取最近秩，每个模式 30 样本时 P99 为该组最大值。脚本还报告每轮 Cold 减 Remote 的 TTFT 差值和 5,000 次配对 Bootstrap 的中位差区间。

原提交复测在 `git archive 02fe2db383bad59dc614d8dd124d21f976ae0869` 解包出的独立目录构建 CUDA Release，运行该提交的 `tests/nanovllm/test_remote_inference.py --device cuda --samples 30 --benchmark <输出路径>`。目录、Worker、模型和词表的参数与现有脚本相同。原版记录的是首次生成之后调度 tick 返回的时间，新版直接取首次采样完成的单调时间戳。当前实现的 Cold、Local 和 Remote 使用相同计时点。原提交的探针组单独运行了 10 个 CUDA Remote 样本。[原提交复测请求](benchmarks/published-validation/README.md) · [原提交逐页计时](benchmarks/published-validation/README.md) · [原路径分段探针](benchmarks/published-validation/README.md)

### TTFT 结果

| 测量组 | Cold P50 / P95 / P99（ms） | Local P50 / P95 / P99（ms） | Remote P50 / P95 / P99（ms） |
| --- | ---: | ---: | ---: |
| CPU Release，独立预热 | 145.45 / 150.63 / 158.17 | 28.30 / 30.36 / 30.84 | **34.36 / 36.21 / 38.12** |
| CUDA Release，独立预热 | 15.67 / 16.05 / 16.38 | 6.93 / 7.28 / 7.40 | **12.67 / 13.18 / 13.22** |
| CUDA Release，匹配原预热设置 | 15.58 / 16.04 / 16.04 | 6.95 / 7.52 / 8.28 | **12.42 / 13.33 / 13.35** |
| 原提交 CUDA Release 复测 | 15.65 / 16.71 / 17.14 | 6.93 / 7.22 / 7.38 | 85.85 / 94.43 / 94.57 |
| 历史 CUDA 数据 | 15.56 / 16.41 / 16.64 | 6.72 / 7.58 / 7.81 | 79.58 / 84.37 / 84.39 |

CUDA 独立预热组的 Remote TTFT P50 下降 19.1%，30/30 对更快，配对中位差 3.03 ms，95% Bootstrap 区间为 2.74–3.13 ms。匹配原预热设置组下降 20.3%，配对中位差 3.16 ms，区间 2.95–3.29 ms。按同一预热设置与历史 Remote P50 对照，79.58 ms 降至 12.42 ms，降幅 84.4%。三个优化组所有 Remote 请求分别导入 5 页、20 Token 和 276,480 Bytes，各用 1 次目录查询和 1 次数据请求，各组 30 次预取均成功。一并复核了优化组和原提交复测组的生成 Token ID 序列 `297 278 4799 29889 2178 278 916 15006`。[CPU 原始样本](benchmarks/published-validation/README.md) · [CUDA 原始样本](benchmarks/published-validation/README.md) · [匹配原预热 CUDA 样本](benchmarks/published-validation/README.md) · [配对统计](benchmarks/published-validation/README.md) · [匹配组配对统计](benchmarks/published-validation/README.md)

### CUDA Remote 阶段耗时

以下为独立预热组，单位均为 ms。累计为 30 次实际请求求和。

| 阶段 | 累计 | P50 | P95 | P99 |
| --- | ---: | ---: | ---: | ---: |
| 预取队列 | 2.962 | 0.089 | 0.158 | 0.160 |
| 目录查询与 Manifest | 30.454 | 0.999 | 1.182 | 1.343 |
| 数据连接获取 | 10.409 | 0.332 | 0.589 | 0.710 |
| 网络接收，含源端等待 | 88.900 | 2.948 | 3.741 | 3.888 |
| CRC 校验 | 1.582 | 0.049 | 0.090 | 0.092 |
| CPU 暂存与排列 | 7.733 | 0.255 | 0.303 | 0.308 |
| CUDA H2D 提交 | 3.393 | 0.095 | 0.245 | 0.423 |
| CUDA Event 等待 | 1.975 | 0.043 | 0.137 | 0.433 |
| 最终 Lease 校验与释放 | 18.509 | 0.571 | 0.918 | 1.147 |
| KV 暂存与本地提交 | 0.379 | 0.013 | 0.014 | 0.014 |
| 调度恢复 | 2.976 | 0.086 | 0.147 | 0.397 |
| 最后提示位置前向至首次采样 | 139.466 | 4.586 | 5.076 | 5.370 |
| 端到端 TTFT | 375.407 | 12.671 | 13.183 | 13.220 |

单次请求的阶段耗时从独立时间区间计算。TTFT 还包括提示编码、其他提示 Token 的前向和阶段间未独立列出的开销。[CPU 全阶段汇总](benchmarks/published-validation/README.md) · [CUDA 全阶段汇总](benchmarks/published-validation/README.md) · [匹配组 CUDA 汇总](benchmarks/published-validation/README.md)

### 消费进程资源

| 设备 | 模式 | CPU 时间 P50（ms） | RSS 峰值 P50（KiB） | GPU 设备占用增量 P50（MiB） |
| --- | --- | ---: | ---: | ---: |
| CPU | Cold | 217.91 | 68,628 | — |
| CPU | Local | 100.28 | 69,024 | — |
| CPU | Remote | 101.74 | 72,908 | — |
| CUDA | Cold | 49.55 | 185,560 | 66 |
| CUDA | Local | 40.73 | 185,520 | 66 |
| CUDA | Remote | 42.48 | 193,828 | 66 |

消费进程 CPU 和 RSS 分别来自 `getrusage` 的时间差和峰值。CUDA 显存使用 `cudaMemGetInfo` 的设备全局差值，WSL 每 PID 显存采样字段保留为空。发布和目录进程使用 `/proc` 的 CPU Clock Tick 和当前 RSS，原始字段及分位数一并保留。

### 环境限制

双机物理网络、RDMA、物理 NVMe 性能和多卡并行未验证。SPDK 配置探测缺少 `spdk_event`、`spdk_env_dpdk` 等 SDK 模块，本轮保留现有 SPDK 后端，运行结果覆盖 POSIX 和 io_uring 的 WSL2 ext4 文件。

设备侧 Compute Sanitizer 无法初始化 WDDM 调试接口，报告 `Device not supported`。CUDA Release 用例在真实 GPU 上执行。ASan/UBSan 的 CUDA 部分覆盖 Host 代码。
