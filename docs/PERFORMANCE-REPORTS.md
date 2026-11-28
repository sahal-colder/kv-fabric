# KV-Fabric 性能报告与原始证据索引

本分支同时发布当前 SPDK 异步改造报告和仓库内已有的全部性能报告、CSV/JSON、环境信息、回归日志及 profiler 采集。历史、预跑、中断和失败样本保留其原有目录及标签，不混入当前正式结果。

## 当前 SPDK / io_uring 同盘结果

- [源码审计](SPDK-ASYNC-IO-AUDIT.md)
- [异步引擎与迁移设计](SPDK-ASYNC-IO-DESIGN.md)
- [实现、兼容边界与复现](SPDK-ASYNC-IO-IMPLEMENTATION.md)
- [正式性能报告](SPDK-ASYNC-IO-BENCHMARK.md)
- [实验总索引](benchmarks/spdk-async-2026-11-28/final-v4/result.json)
- [正式性能曲线](benchmarks/spdk-async-2026-11-28/final-v4/overview.png)
- [CUDA / SSD 跨 Page 流水线图](benchmarks/spdk-async-2026-11-28/final-v4/pipeline-trace4/pipeline-overlap.png)
- [独立发布副本的 48 项 Release/CUDA/SPDK 回归](benchmarks/spdk-async-2026-11-28/publication-clean-build-ctest.log)

实测双方均达到真实 QD128。512 KiB 随机读 QD32，SPDK/io_uring 为 2210.4/1680.5 MiB/s；128 Page GPU Recovery 并发16为1.807/1.627 GiB/s。SPDK消耗更多轮询CPU，小块峰值和写入没有全面优势。完整数据与限制以正式报告为准。

## 历史性能报告

- [原生 Linux CUDA/NVMe 验证](benchmarks/native-linux-final/)
- [性能归因分析与 Nsight 采集](benchmarks/perf-attribution-2026-11-26/)
- [同盘 SPDK / io_uring 对照](benchmarks/spdk-uring-2026-11-28/)
- [SPDK 流水线调优](benchmarks/spdk-pipeline-2026-11-28/)
- [物理 NVMe 恢复与验证](SPDK-NVME-VALIDATION.md)
- [RDMA 跨实例报告](RDMA-CROSS-INSTANCE-AUDIT.md)
- [RDMA 原始数据](benchmarks/rdma-cross-instance-2026-11-26/)
- [审计与回归](KV-FABRIC-AUDIT-REMEDIATION.md)
- [项目验证报告](KV-FABRIC-VALIDATION-REPORT.md)
- [优化测试报告](KV-FABRIC-ULTIMATE-TEST-REPORT.md)
- [全部实验目录](benchmarks/)

各根目录报告的清单及 SHA-256 见 [发布清单](benchmarks/publication-inventory.json)。

## 大型原始文件的无损压缩

大型CSV、SQLite、Nsight/Nsight Compute采集使用gzip无损压缩；过大的压缩文件按40 MiB分卷，并打包到Release附件。Markdown、图表、汇总CSV/JSON和小型样本可直接浏览。没有抽样删除正式轮次，也没有用汇总替换原始样本。

[archive-index.json](benchmarks/archive-index.json)逐文件记录原路径、原字节数、原SHA-256，以及压缩文件/分卷SHA。附件解包后，原始文件的压缩版本位于同目录，名称为原文件名加`.gz`，或`.gz.part000`等。

大型压缩文件作为[GitHub Release附件](https://github.com/sahal-colder/kv-fabric/releases/tag/spdk-async-io-20261128)发布，Git仓库保留逐文件索引。附件清单、字节数和SHA见[release-assets.json](benchmarks/release-assets.json)。

```bash
# 在仓库根目录下载、验证并解包大型采集附件。
gh release download spdk-async-io-20261128 --repo sahal-colder/kv-fabric \
  --pattern 'kvfabric-performance-*.tar' --pattern SHA256SUMS
sha256sum -c SHA256SUMS
for archive in kvfabric-performance-*.tar; do
  tar -xf "$archive"
done

# 只恢复本轮 SPDK 的大型原始文件；每个文件均验证原始 SHA。
python3 scripts/restore_benchmark_archives.py --group spdk-async-2026-11-28

# 恢复全部历史原始文件，供 SQLite/Nsight 或统计脚本使用。
python3 scripts/restore_benchmark_archives.py
```

恢复工具不联网、不下载模型、不访问块设备。恢复后会产生较大的原始文件，分析工具的既有路径无需修改。

## 发布范围与源码归属

本分支包含异步SSD引擎及其已存在的Storage生命周期、Control/Remote/Nano-VLLM依赖，以保证可以独立构建和测试。KVStore的Reactor、Proactor、NtyCo实现及`start_network()`不在本次上传中改写；原工作区的其他未提交KVStore修改留在原工作区。

实测开始时工作区已有未提交修改，历史源码快照和SHA清单按原样保留。发布副本的构建路径、Git换行规范和与SSD无关的KVStore源码可能不同于实测工作区，不能将历史SHA清单误当成当前Git提交的全仓库SHA；实际使用的源码、工具与实验口径以各run.json和源码快照为准。

磁盘原始备份、真实模型权重、编译对象/临时执行文件和私人简历生成工具不上传。生成式小模型fixture可由已有脚本重新生成；原SHA仍保留。被排除文件的路径和原因见[排除清单](benchmarks/publication-exclusions.json)。

物理设备测试保持手动运行，不注册默认CTest；克隆后不能直接照搬本机设备、序列号或测试区域执行破坏性写入。
