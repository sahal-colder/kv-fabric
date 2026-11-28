# SPDK 物理 NVMe 验收（2026-11-28）

本轮在原生 Ubuntu 主机上完成 SAMSUNG MZVLQ512HALU-00000 的真实 SPDK PCIe 读写。设备容量 512110190592 字节（476.94 GiB），PCI 地址 `0000:07:00.0`，独占 IOMMU 组 16，SPDK bdev 名为 `Samsungn1`。当前整块盘由 `vfio-pci` 管理，供部署时指定的用户使用；SPDK 直接访问 bdev，无文件系统挂载点。

未检测到分区表或文件系统签名。测试前备份了盘首/盘尾各 1 MiB，以及从 1 GiB 开始的 128 MiB 测试区域。所有测试写入均局限于该区域，测试完成后恢复原始内容，并切回内核驱动、刷新缓存、重新读取校验三处 SHA-256。系统 UMIS 盘保持原驱动与挂载。

## 启动问题与修复

SPDK SDK 是 v27.01-pre `4828014`，DPDK 25.11.0。该主机的 DMAR Host address width 为 39 位；SDK 默认虚拟地址 `0x200000000000` 超出其范围，因此自动选择 PA。普通用户无法取得物理地址，导致 `Cannot use IOVA as 'PA'`。只设置 VA 仍会触发 `IOVA exceeding limits of current DMA mask`。

桥接层现在支持 `mi_spdk_config.iova_mode` 和 `base_virtaddr`；CLI 对应 `--spdk-iova-mode`、`--spdk-base-virtaddr`。实测使用 VA 与 `0x1000000000`（64 GiB）起点，兼容本机 IOMMU，也避开 CUDA 的低地址保留区。原有零初始化配置继续保留 SDK 默认值。`no_huge` 路径由 SDK 自动添加 VA，桥接层避免重复参数，并拒绝 PA 与 `no_huge` 的非法组合。

## 本机部署

`kvfabric-spdk-samsung.service` 已启用开机启动，安装的管理程序是 `/usr/local/lib/kvfabric/spdk_device.py`，源码见 `scripts/spdk_device.py`。服务在每次新启动时按序列号查找设备，检查挂载、swap、占用和 IOMMU 隔离，分配至少 512 MiB 大页，然后仅绑定目标盘。VFIO 节点仅允许目标用户访问。设备 JSON 按实际 PCI 地址生成至 `/etc/kvfabric/spdk-samsung.json`，设备状态保存在 `/var/lib/kvfabric-spdk/device.json`。

服务已完成启动、停止、内核驱动恢复、数据校验和再次启动验证；没有重启整台主机。服务还设置满足 PCI uevent 监听要求的 `net.core.rmem_max`，避免 SPDK 接收缓冲区不足。验收用户的 memlock 上限约 1964 MiB，足以容纳 256 MiB 的 SPDK 应用内存。

```bash
systemctl status kvfabric-spdk-samsung.service
sudo systemctl stop kvfabric-spdk-samsung.service   # 先退出使用该盘的 SPDK 程序
sudo systemctl start kvfabric-spdk-samsung.service
```

停止服务会将设备交回内核 `nvme` 驱动。开机自动接管可用 `sudo systemctl disable --now kvfabric-spdk-samsung.service` 撤销。管理程序不格式化、不分区，也不向裸盘写入测试数据。

## 构建与运行

`build-spdk` 和现有 `build-cuda-release` 均已启用 SPDK、CUDA 与 io_uring。`build-cuda-release` 同时保留 vLLM CUDA bridge。

```bash
cmake -S . -B build-cuda-release -DCMAKE_BUILD_TYPE=Release \
  -DMINIINFER_WITH_SPDK=ON -DMINIINFER_WITH_CUDA=ON \
  -DNANO_WITH_CUDA=ON -DMINIINFER_WITH_URING=ON \
  -DMINIINFER_BUILD_VLLM_BRIDGE=ON -DMINIINFER_VLLM_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-cuda-release -j6
ctest --test-dir build-cuda-release --output-on-failure

build-cuda-release/bin/miniinfer /path/to/model.bin -z /path/to/tokenizer.bin \
  --device cuda --stdin --ssd-mode spdk --ssd-slots 32 \
  --spdk-config /etc/kvfabric/spdk-samsung.json --spdk-bdev Samsungn1 \
  --spdk-offset 1073741824 --spdk-iova-mode va \
  --spdk-base-virtaddr 0x1000000000
```

模型与 `--block-size` 决定每槽大小；槽数乘以包含头部的 4096 字节对齐槽步长须小于等于预留的 134217728 字节。上述模型路径是占位符；本轮推理验证使用本地生成的确定性小模型 fixture。

显式硬件工具 `kvf_spdk_verify CONFIG BDEV OFFSET CAPACITY MODE [BACKUP]` 提供 `raw`、`cpu`、`cuda`、`restore` 模式，不自动加入 CTest。`raw` 在写入前确认区域与原始备份相同，完成后恢复并逐字节核对。`cpu` 和 `cuda` 会写入调用者指定的区域，运行后须执行 `restore`；该工具不负责建立备份或识别磁盘用途。

## 验收结果

| 项目 | 结果 |
|---|---|
| 普通用户打开真实 PCIe NVMe | PASS，VFIO + VA + 64 GiB 地址起点 |
| 裸盘连续读写 | 128 MiB × 5 轮，逐字节一致 |
| 非对齐读改写 | 偏移 19 字节、37 字节头部、多段 DMA；整个区域比较一致 |
| CPU → SPDK → CPU | 32 × 512 KiB × 5 轮，记录身份、CRC、版本和载荷一致 |
| GPU → Host → SPDK → Host → GPU | 同等载荷与轮数，真实 CUDA 搬运，全部一致 |
| 推理 A → B → A | SSD 写入 2 次、读取 1 次；第三次复用 32 Token，输出 Token ID 与基线一致 |
| 关闭与内核驱动切回 | PASS，服务停止和再次启动均成功 |
| 测试区域恢复 | PASS，内核路径重新读取原始 SHA-256 一致 |
| CUDA + io_uring + SPDK 全量 CTest | 26/26 PASS，44.93 秒 |

原始记录和备份位于本机忽略目录 `.kvfabric-local/spdk-samsung/`。测量时的主机、驱动和设备信息保留在本地，不将数据备份加入版本控制。汇总结果见同目录 `result.json`。

## 简历复验 30 次实测

2026-11-28 另建独立归档执行 3 次预热、30 次测量，每次迁移 32 个 512 KiB 块（16 MiB）。CPU 与 GPU 两条路径各核验 480 MiB 测量载荷；逐块检查 SSD 驻留、版本、CRC 与回源字节。GPU 回源计时包括 SPDK 读取、CRC 校验、Host Staging、H2D 及最终同步，计时后的快照逐字节检查独立执行。

| 路径 | 样本数 | 回源 P50 | 回源 P95 |
|---|---:|---:|---:|
| SPDK → CPU | 30 | 17.55 ms | 21.25 ms |
| SPDK → GPU | 30 | 18.95 ms | 23.18 ms |

P50 为中位数，P95 为 nearest-rank，样本规模为 30。数值来自独立分层回源测试，未与历史 Qwen TTFT 或其他容量的文件回源作加速比比较。裸盘 128 MiB × 5 轮和非对齐读改写再次通过；A→B→A 小模型推理观测到 2 次写入、1 次读取，与基线 Token ID 严格一致；全量 CTest 再次 26/26 PASS（42.78 秒）。测试区域已再次恢复并通过 SPDK 逐字节回读。

脱敏[汇总](benchmarks/spdk-nvme-2026-11-28/summary.json)、[CPU 样本](benchmarks/spdk-nvme-2026-11-28/cpu.csv)与[GPU 样本](benchmarks/spdk-nvme-2026-11-28/cuda.csv)保留全部测量值及源码 SHA-256。硬件工具为 `cpu` / `cuda` 模式提供 `--rounds 30 --warmup 3` 参数；原始完整日志位于 `.kvfabric-local/spdk-samsung/resume-validation-20261128/`。

本轮证明物理 SPDK 读写及本项目存储/推理接入可用。GPU 路径经过 Host staging；实测不代表 GDS、最大 NVMe 吞吐或生产 SLA。运行中物理拔盘、设备掉电及在线故障恢复尚未验证。
