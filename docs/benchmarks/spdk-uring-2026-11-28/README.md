# 原盘对照原始数据

完整方法、表格、复现命令、限制、恢复哈希和中文简历表述见 [SPDK-IOURING-COMPARISON.md](../../SPDK-IOURING-COMPARISON.md)。

- `b*-*-qd*-*.csv`：32 个进程，每进程 3 预热+30 正式轮；微基准全部样本。
- `run.json`：实际命令顺序、返回码、模型哈希、原盘身份和恢复证据。
- `infer-b*-*.txt` / `inference.csv`：两种已接通配置的小模型重复实测。io_uring 裸盘推理尚未接通，没有捏造第三组样本。
- `summary.json`：由 `scripts/summarize_spdk_uring_compare.py` 重算，可在不操作设备的情况下复核。
- `sample-audit.json`：极端样本保留清单，无删样。
- `historical-cuda-replay.*`：历史逐块同步 SPDK 代码的本轮重测，独立于主对照。
- `measured-source.tar.gz` / `source-sha256.json`：包含既有未提交依赖的实际源码归档及哈希；原盘备份、模型二进制和密码均不在归档中。
- `source-rebuild.txt`：从归档解出的源码独立重建三个实测目标的成功日志。

两组使用独立路径：io_uring 基线经过内核 nvme 驱动；SPDK 使用 vfio-pci + PCIe NVMe bdev，直接绕过内核块层，没有经过 io_uring。

GPU 最优 QD=4：io_uring 基线 P50/P95 为 9.661468/10.086211 ms；独立 SPDK 路径为 9.890206/10.385186 ms。该配置没有 SPDK 加速收益。
