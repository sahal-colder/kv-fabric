# KV-Fabric 数据面部署与复测

```text
                   kvfabricd
                ↗ 元数据 RPC ↖
      Producer Worker ─── TCP Layer ─── Consumer Worker
      CUDA 显存 → Segment                 Segment → 主机内存
                  ↕                         ↕
              ext4 文件                 本地 Prefix Trie
```

## 构建

在两台 Linux 机器上准备同一版本的 `kvstore/` 和 `miniinfer-c/`，安装
C11 编译器、CMake、OpenSSL 开发包。启用 io_uring 时还需 liburing 开发包。
以下命令从两个项目的父目录执行。可按部署环境修改 `prefix`。

```sh
prefix="$HOME/.local/kvfabric"
scratch=$(mktemp -d /tmp/kvfabric-build.XXXXXX)
cmake -S kvstore/kvfabric -B "$scratch/control" -DCMAKE_BUILD_TYPE=Release
cmake --build "$scratch/control" -j
ctest --test-dir "$scratch/control" --output-on-failure
cmake --install "$scratch/control" --prefix "$prefix"
```

Node A 使用 CUDA Runtime，已安装多个 CUDA 工具包时可设置 `CUDAToolkit_ROOT`。

```sh
cmake -S miniinfer-c/products/miniinfer-storage -B "$scratch/worker" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$prefix" \
  -DMINIINFER_WITH_KVFABRIC=ON -DMINIINFER_WITH_CUDA=ON -DMINIINFER_WITH_URING=ON
cmake --build "$scratch/worker" -j
cmake --install "$scratch/worker" --prefix "$prefix"
```

Node B 默认使用 CPU。在 Node B 构建并安装相同版本的控制面 SDK 后，
用以下命令构建 Worker。如果 Node B 也使用 CUDA，沿用 Node A 的构建选项。

```sh
cmake -S miniinfer-c/products/miniinfer-storage -B "$scratch/worker" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$prefix" \
  -DMINIINFER_WITH_KVFABRIC=ON -DMINIINFER_WITH_URING=ON
cmake --build "$scratch/worker" -j
ctest --test-dir "$scratch/worker" --output-on-failure
cmake --install "$scratch/worker" --prefix "$prefix"
ldd "$prefix/bin/miniinfer_worker"
```

## 双机演示

用 Node A 的真实 LAN IPv4 替换示例里的 `192.168.1.20`，在可信局域网内
允许 Node B 访问该地址的 TCP 9000 和 9001。两边使用相同的每层字节数。
Node A 保持 Producer 运行，在 Node B 运行 Consumer。

```sh
# Node A
bash miniinfer-c/scripts/run_hetero_demo.sh producer "$prefix" 192.168.1.20 /tmp/kvf-demo 1048576

# Node B
bash miniinfer-c/scripts/run_hetero_demo.sh consumer "$prefix" 192.168.1.20 /tmp/kvf-demo 1048576 100
```

Producer 默认用 `cuda:0`，Consumer 默认用 `cpu`。可用
`KVF_PRODUCER_DEVICE`、`KVF_CONSUMER_DEVICE` 指定 `cpu`、`cuda` 或
`cuda:ID`。需要给其他机器提供 Consumer
已导入的副本时，将 `KVF_CONSUMER_LISTEN` 设为 Node B 的可达地址和端口，
例如 `192.168.1.21:9002`。可通过 `KVF_CONTROL_PORT` 与
`KVF_DATA_PORT` 修改默认端口。

本机 smoke test 使用两个终端、同一安装目录和显式 loopback。

```sh
KVF_PRODUCER_DEVICE=cpu bash miniinfer-c/scripts/run_hetero_demo.sh producer \
  "$prefix" 127.0.0.1 /tmp/kvf-demo 4096
KVF_CONSUMER_DEVICE=cpu bash miniinfer-c/scripts/run_hetero_demo.sh consumer \
  "$prefix" 127.0.0.1 /tmp/kvf-demo 4096 10
```

脚本在两端各自生成相同的 2 页 × 2 层 FP32 K/V 数据。Consumer 从本地
目标设备回读后，逐字节比较 Tensor、SHA-256 和两层的标量 FP32 Attention
结果。Attention 的参考与导入结果在 Consumer 同机计算。报告 P99 和
有效载荷吞吐，范围为一次完整前缀的远程解析和导入。进程初始化、回读与
Attention 检查在计时区间之外。每轮仍须通过检查才会写入 `latency.csv`。

## 文件层与故障语义

- 为 Producer 设置 `MINIINFER_WORKER_SSD=/path/on/ext4/kvf-scratch` 可将
  已封存的层迁到 SSD，再发布到目录。文件后端会重置指定路径，传入专用缓存文件。
- 对齐文件记录的元数据区和负载步长都是 4 KiB 的倍数。记录保存 Magic、
  Block ID、Version、Generation、长度以及两种 CRC32C。每个并发 I/O 槽的
  Ring 在创建时注册一次缓冲区，之后使用 READ_FIXED 和 WRITE_FIXED。
- CUDA 设备与 SSD 的整块迁移直接使用同一槽负载做 DMA 与定位 I/O。SSD
  来源的 TCP Fetch 会将已校验的块复制到网络发送槽。CPU 分配为对齐内存，
  固定 I/O 的页由内核在注册时锁定。CUDA staging 使用 `cudaMallocHost`。
- 运行时输出实际 I/O 模式。固定缓冲区不可用时回退为对齐的 O_DIRECT
  POSIX。O_DIRECT 打开失败时用对齐格式的 buffered POSIX。
- 记录校验失败或传输中断时丢弃暂存页，已提交前缀继续可用。进程从空
  Trie 启动，重新从活副本拉取或重算。目录重启分配新 Epoch，后台心跳
  恢复本地已发布副本。

## Benchmark

先用 `findmnt -T` 与 `lsblk` 确认目标 ext4 对应的实际设备。WSL 的 ext4
虚拟盘可验证文件路径和 I/O 模式。物理 SSD 性能需在原生 Linux 主机上测量。
脚本拒绝 9p、NTFS 等非 ext4 目录。32 × 4 MiB 的 fixed-buffer 组建议给
运行进程至少 256 MiB 的 memlock 额度，以免内核拒绝注册。

```sh
KVF_SSD_ROOT=/tmp/kvfabric-ssd KVF_BENCH_DEVICES='cpu cuda' \
  bash miniinfer-c/products/miniinfer-storage/tools/run_kvf_benchmark.sh \
    "$prefix/bin/kvfabricd" "$prefix/bin/miniinfer_worker" "$prefix/bin/kvf_bench" \
    "$PWD/miniinfer-c/docs/benchmarks"
```

`KVF_BENCH_DEVICES=cpu` 仅跑 CPU TCP，空值跳过 TCP。设置
`KVF_BENCH_SSD=0` 跳过文件测试。两条模式使用固定数据、每线程四次预热，
每组合三轮、每轮每线程 16 次校验。TCP 计时包含两层申请、导入、释放及
回读校验。文件基准计时包含一块写入、读回、校验和释放。CPU、RSS 和
各延迟分位数见逐轮 CSV。 [实测报告](KV-FABRIC-IMPLEMENTATION-REPORT.md)
