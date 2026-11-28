# 原盘异步流水线实测

完整说明见 [SPDK-PIPELINE-OPTIMIZATION.md](../../SPDK-PIPELINE-OPTIMIZATION.md)。

- `b*-{powersave,performance}-{uring,spdk}-{serial,pipeline}-qd*.csv`：80 个进程，每进程 3 预热+30 正式，合计 2640 轮。
- 同名 `.txt`：全部数据/版本/CRC/主机及 GPU 字节校验标志和 SDK 日志。
- `run.json`：实际顺序、完整命令、返回码、进程生命周期 user/system CPU 时间、原备份和前后恢复哈希、governor/设备/服务状态。
- `summary.json`、`sample-audit.json`：可用 `scripts/summarize_spdk_pipeline_tune.py` 重算，全部样本保留。
- `measured-source.tar.gz`、`source-sha256.json`、`source-rebuild.txt`：实际工作树源码、校验值和独立构建证据。

主结论：默认 powersave、同 QD4，SPDK 串行对比流水线，P50 9.789855 / 7.345553 ms，P95 10.287459 / 7.615879 ms。相同流水线的 io_uring P50 为 7.390797 ms。约 25% 的降延迟属于流水线收益；SPDK 额外 P50 优势约 0.61%。
