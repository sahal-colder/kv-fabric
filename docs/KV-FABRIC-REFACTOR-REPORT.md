# KV-Fabric 重构与验证报告

在 `dev` 上完成清理，对比起点为 `02fe2db383bad59dc614d8dd124d21f976ae0869` 上开始时的工作区。纯 HEAD 也已单独复测，详见审计。包含批量传输、双端租约检查、pinned staging 和异步发布的原工作区快照是零回退比较的基准。[全仓审计](KV-FABRIC-CODE-AUDIT.md)

## 删除与合并

| 删除文件 | 承接位置和删除依据 |
| --- | --- |
| `docs/miniinfer-storage.md` | 独立构建、示例和测试命令移入 `docs/miniinfer.md#standalone-storage`。文档内部链接已改指新目录。 |
| `src/control/crc32c.c` | 复用 `third_party/infer-io/src/base/crc32c.c` 的软件和 SSE4.2 核心。Control 和单机 Storage 各自保留原 CRC 符号与硬件探测方式，更新了 CMake 与阅读指南。 |
| `src/worker/cuda_adapter.c` | 兼容符号并入 `src/worker/device_transfer.c`。公开头文件及 CPU 构建下的 `MI_EUNSUPPORTED` 返回语义保留，CMake 与教程已跟进。 |

| 调用链 | 实际变更和约束 |
| --- | --- |
| Wire 与 Directory | `kvf_put_bytes` / `kvf_get_bytes` 共用无下溢范围检查。普通查询与授权共用候选参数检查。Wire 布局、帧 CRC、父链、目录 Epoch、源端 Boot、Fence/Secret 及 Generation/Version 校验保持原语义。 |
| Remote Worker | Worker 按已启动阶段回收线程，移除私有 `heartbeat_started`。Prefetch 拒绝和已领取 Ticket 的销毁共用释放。兼容单层回复共用发送点，缓存空间重试共用解锁出口。连接池相同地址才比较 Boot，健康复用时沿用原身份。 |
| Storage 与 CUDA | Segment 槽的对齐基地址降为构造局部变量。生产 CUDA Runtime 进入 `device_cuda.cu` 和 `nano_cuda.cu`，Host C 源使用 `-std=c11`。CPU 保留原对齐分配和搬运。RoPE 使用现有分配函数，在原 Stream 同步点后释放。仅开启 Storage CUDA 的构建也使用 CUDA 编译器。 |
| Nano-VLLM | 取消和抢占复用活动请求退出路径。K/V 地址共用同一偏移式，SSD 和 Remote 布局共用每页元素数。远程发布优先复用该物理页已缓存的 SHA-256，缺失才计算。调度预算、最后提示 Token、导入提交与发布重试顺序保留。 |

批量收发、私有暂存、相邻页 H2D、单一完成 Event、准入成本模型、异步发布撤销及 TLSF 核心均保留原工作区实现。新清理没有增加每次请求的分配、复制、同步或全局锁。批量部分原有的页字节计算和 Event 错误路径仍可单独整理，本轮保留了对应未提交补丁的行。

## 工作区增量

与任务开始快照相比，原有内容的重构共 30 个路径，修改 26 个，新增 1 个，删除 3 个。含构建文件和既有文档的净变化为 +349 / −423，净减 74 行。仅计 C/CUDA 源码和头文件为 +320 / −394，净减 74 行。交付审计、技术报告、原始 CSV 和验证件单独存放。

| 状态 | 文件 | 新增行 | 删除行 |
| --- | --- | ---: | ---: |
| 修改 | `CMakeLists.txt` | 6 | 4 |
| 修改 | `docs/READING-GUIDE.md` | 2 | 2 |
| 删除 | `docs/miniinfer-storage.md` | 0 | 16 |
| 修改 | `docs/miniinfer.md` | 15 | 1 |
| 修改 | `docs/源码学习教程.md` | 1 | 1 |
| 修改 | `include/miniinfer/device_transfer.h` | 7 | 0 |
| 修改 | `src/control/CMakeLists.txt` | 3 | 2 |
| 删除 | `src/control/crc32c.c` | 0 | 120 |
| 修改 | `src/control/directory.c` | 6 | 2 |
| 修改 | `src/control/wire.c` | 6 | 2 |
| 修改 | `src/nanovllm/cache.c` | 6 | 2 |
| 修改 | `src/nanovllm/cli.c` | 1 | 1 |
| 修改 | `src/nanovllm/engine.c` | 17 | 25 |
| 修改 | `src/nanovllm/nano_cache.h` | 5 | 0 |
| 修改 | `src/nanovllm/nano_cuda.cu` | 6 | 5 |
| 修改 | `src/nanovllm/nano_remote_kv.c` | 4 | 3 |
| 修改 | `src/nanovllm/nano_spill.c` | 1 | 1 |
| 修改 | `src/worker/CMakeLists.txt` | 2 | 3 |
| 删除 | `src/worker/cuda_adapter.c` | 0 | 37 |
| 新增 | `src/worker/device_cuda.cu` | 151 | 0 |
| 修改 | `src/worker/device_transfer.c` | 59 | 159 |
| 修改 | `src/worker/remote/cache.c` | 4 | 6 |
| 修改 | `src/worker/remote/internal.h` | 0 | 1 |
| 修改 | `src/worker/remote/peer.c` | 8 | 7 |
| 修改 | `src/worker/remote/prefetch.c` | 8 | 6 |
| 修改 | `src/worker/remote/transfer.c` | 3 | 2 |
| 修改 | `src/worker/remote/worker.c` | 6 | 12 |
| 修改 | `src/worker/segment_pool.c` | 2 | 2 |
| 修改 | `src/worker/segment_pool.h` | 0 | 1 |
| 修改 | `third_party/infer-io/src/base/crc32c.c` | 20 | 0 |

初始工作区的 31 个跟踪文件修改和 19 个未跟踪文件独立记录。其中 13 个文件同时涉及本次重构，前后行级对照确认新增改动与原补丁不重叠。其余 37 个路径逐字节一致，含三个 NtyCo 文件。分支、提交和暂存区未改变。[原有状态](benchmarks/refactor/baseline/git-status-before.txt) · [原始 SHA-256](benchmarks/refactor/baseline/workspace-before.csv)

## 构建和测试

| 配置 | 结果 |
| --- | --- |
| `build-refactor-cpu` | CPU Release，14/14 CTest |
| `build-refactor-cuda` | 启用 Worker 和 Nano CUDA 的 Release，17/17 CTest，真实 RTX 5060 Ti |
| 关闭 KV-Fabric | 独立 CPU Storage 与 Nano，7/7 CTest |
| 关闭 Nano 和集成 CLI，仅启用 Worker CUDA | 15/15 CTest |
| CPU ASan / UBSan / LSan | 14/14 CTest，泄漏检测开启 |
| CUDA Host ASan / UBSan / LSan | 17/17 CTest，开启泄漏检测，使用 WSL 驱动库单项抑制 |
| CPU TSan | 14/14 CTest，`setarch x86_64 -R`，`halt_on_error=1` |
| KVStore Make 构建 | 单元与分配器检查、协议回归、TCP 复制和可靠性测试通过。`NTYCO=1` 的可靠性结果来自含原有 NtyCo 补丁的本地快照。 |
| POSIX / io_uring | CPU/CUDA 各跑两种后端 × 开关 Remote 的 8 个组合，每组 5 页回读、9 页写出，Token ID 与同设备无缓存对照一致。 |

CPU 和 CUDA 的 `kvfabric`、`miniinfer_storage`、`miniinfer_remote`、`miniinfer_nano` 静态库以 `nm` 比较，项目 C 符号集合一致。两组 Release 的所有 C 翻译单元均使用 `-std=c11`。CUDA Host Sanitizer 使用 `protect_shadow_gap=0` 和只匹配 `/usr/lib/wsl/lib/libcuda.so.1` 的抑制。CPU 的 LSan 使用默认泄漏检查且无抑制。完整 Sanitizer 日志保留在验证目录。[构建和测试结果](benchmarks/refactor/after/validation/)

## 配对性能

每个设备与模式比较 100 轮，奇数轮先运行基准侧，偶数轮反序。同轮使用同一模型、词表、输入、采样参数和冷/本地/远程顺序。两侧分别启动独立 Directory 和发布 Worker，每个消费进程预热一条无关请求。工具按最近秩计算 P50/P95/P99，同时保留每轮 `(after / baseline - 1) × 100%` 和配对中位变化。阈值为 P50 ≤ +3%，P95/P99 ≤ +5%。

| 设备 | 模式 | Baseline P50 / P95 / P99（ms） | After P50 / P95 / P99（ms） | 分位变化 | 配对中位变化 |
| --- | --- | ---: | ---: | ---: | ---: |
| CPU | Cold | 142.161 / 148.806 / 153.247 | 142.918 / 149.812 / 153.031 | +0.53% / +0.68% / -0.14% | +0.28% |
| CPU | Local | 27.892 / 29.995 / 32.549 | 28.059 / 30.031 / 30.743 | +0.60% / +0.12% / -5.55% | +0.76% |
| CPU | Remote | 34.108 / 36.053 / 36.989 | 34.184 / 36.240 / 37.192 | +0.22% / +0.52% / +0.55% | +0.79% |
| CUDA | Cold | 15.718 / 16.648 / 17.072 | 15.822 / 17.055 / 17.306 | +0.66% / +2.45% / +1.37% | +0.36% |
| CUDA | Local | 6.773 / 7.210 / 7.445 | 6.909 / 7.453 / 7.600 | +2.01% / +3.37% / +2.08% | +1.76% |
| CUDA | Remote | 12.703 / 13.796 / 14.129 | 12.621 / 13.569 / 14.425 | -0.64% / -1.64% / +2.09% | -0.70% |

两设备各 100 轮配对得到的 18 项 TTFT 分位数均在阈值内。每侧 Remote 样本均为一次数据 RPC、276,480 Bytes、5 页和 20 个复用 Token，生成的 8 个 Token ID 逐轮相同。CUDA Remote 调度恢复 P99 从 0.144 ms 到 0.139 ms。

复现时用同一套编译选项分别从原工作区快照和最终源码构建 `--before`、`--after` 指定的目录。模型和词表必须匹配脚本记录的 SHA-256。`--harness` 指向含 `test_remote_inference.py` 的目录。运行时只测一个设备及一侧进程组。更多环境与二进制 SHA-256 见样本同目录。

~~~bash
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1
python3 docs/benchmarks/refactor/run_paired.py \
  --device cuda --rounds 100 --before "$BEFORE_CUDA" --after "$AFTER_CUDA" \
  --model "$MODEL" --tokenizer "$TOKENIZER" \
  --harness tests/nanovllm --out "$OUTPUT"
~~~

[同轮基准原始数据](benchmarks/refactor/baseline/paired/) · [重构后原始数据、逐轮变化和分位数](benchmarks/refactor/after/)

## 环境边界

性能测试使用 Ubuntu 22.04 WSL2、单台主机的 IPv4 TCP 回环和 RTX 5060 Ti。ASan、UBSan 和 LSan 的 CUDA 构建覆盖 Host 代码。当前 SPDK 配置探测缺少 `spdk_event`、`spdk_env_dpdk` 等 SDK 依赖。SPDK 运行路径、物理 NVMe 性能、双机网络、RDMA、多卡和设备侧 Sanitizer 均未验证。[SPDK 探测日志](benchmarks/refactor/after/validation/spdk-configure.txt)
