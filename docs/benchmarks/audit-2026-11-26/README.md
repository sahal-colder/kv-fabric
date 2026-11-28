# 2026-11-26 本地审计验收证据

该目录是本轮实际运行结果；历史失败也保留。主要结论见 [验收报告](../../KV-FABRIC-VALIDATION-REPORT.md) 和 [逐项账本](../../KV-FABRIC-AUDIT-REMEDIATION.md)。路径中 before/after 表示测量源码版本，不表示所有补测都发生在源码修改之前。

## 入口

- `baseline-*-ctest.*`：保护快照下的原始构建/测试。无后缀 local 的初次 CPU 结果含沙箱 EPERM，不能作为源码基线失败。
- `final-*-ctest.*`、`tsan-ctest.*`、`tcp-ctest.*`、`storage-only-ctest.log`：最终验证；最新测试专用 uring 三重错误缝隙还保存为 `final-uring-{asan,tsan,quarantine}.log`。
- `tsan-first-run.*`：新增测试夹具竞态的真实失败；修后全量及核心三轮结果另存，没有 suppression。
- `final-cuda-*-memcheck.log`：真实 GPU memcheck，Direct 使用 direct-uring-fixed。
- `legacy-*.log`、`final-legacy-*.log`：Legacy 构建、单元、认证/防重放、安全和三种网络后端可靠性；MMPOOL 初始夹具未初始化问题与修正记录在账本。
- `reproduced-before.log`、`reproduced-remote-before.log`：冻结原始库实际复现 #1/#2/#3/#4/#11。
- `kvf_bench_before.txt`、`kvf_bench_after.txt`：Phase 0 / 最终同参数单轮命令与输出。所有中间性能结果也保留。
- `performance-final-single.json`、`performance-paired.json`、`performance-accepted-recovery.json`：对应验收报告的原始与交替比较。`performance-paired.json` 的恢复指标是修复额外拷贝前的中间版本；最终恢复见 accepted 文件。
- `before/`、`after-accepted/`：64 KiB、4客户端 SSD/Remote 原始样本。`paired-*/` 为8对CPU及4对CUDA；`accepted-cuda-*/` 为最终4对恢复。`final-cuda-*/` 是此前6对恢复，不与 accepted 重复合并。
- `nano-*.csv`：固定合成模型12轮/设备/版本的Cold/Local/Remote与逐页样本；不是生产模型数据。
- `preexisting-files.json`、`external-updates.json`：用户原有文件哈希与外部并发改动；`audit-changed-files.json`：本轮代码/文档/配置清单，排除本证据目录；`tested-binaries.json`：实际二进制哈希。
- `artifact-manifest.json`：本目录全部文件SHA-256清单（不含清单自身）。`audit-source.diff`：相对启动工作区的本轮文本差异，供复核，不包含用户原补丁与6个外部文件更新。

## 复现

核心CMake/Make命令在验收报告中。测试用例已纳入仓库构建，fault hooks只在测试目标启用。

性能驱动 `repro/run-bench.py` 使用本仓库构建、实际ext4私有文件和loopback服务，参数与本轮一致。第二参数必须是不存在的新输出目录，避免在旧CSV中追加：

```bash
python3 docs/benchmarks/audit-2026-11-26/repro/run-bench.py build-release /tmp/kvf-audit-new-run
```

`repro/micro.c` 是Phase 0的原始临时微基准；`repro/original-run-bench.py` 保留当时确切驱动，portable版本只改变路径/输出目录管理。历史 `paired.py`、`nano-paired.py`、`accepted-recovery-paired.py` 保留当时的绝对路径和命令，审阅后在新目录中运行，避免覆盖旧数据。

GPU原始入口（当前机器sm_89，必须可访问驱动）：

```bash
build-cuda-release/bin/kvf_cuda_bench all
mkdir /tmp/kvf-audit-new-recovery
build-cuda-release/bin/kvf_nvme_recovery_bench /tmp/kvf-audit-new-recovery direct-uring-fixed
compute-sanitizer --tool memcheck --error-exitcode 99 --leak-check full build-cuda-release/bin/test_direct_io cuda
```

SPDK仅使用malloc bdev，不接触原始块设备：

```bash
cmake -S . -B build-spdk -DCMAKE_BUILD_TYPE=Release -DMINIINFER_WITH_SPDK=ON
cmake --build build-spdk -j6
ctest --test-dir build-spdk -R 'spdk_' --output-on-failure
```

原始完整工作区归档仍位于 `/tmp/kvf-audit/preexisting-files.tar`（约1.47 GB），没有复制进仓库。HEAD+该归档可在新目录恢复启动工作区业务源码；保留的 `build-baseline` CPU二进制及 `/tmp/kvf-audit/original-cuda` 是本次 before 数据来源。禁止在当前工作区用它们覆盖用户文件。环境、构建选项与二进制哈希独立记录，不能仅凭build目录名判断源码版本。

## 用户授权后的补齐与发布

`publish-*-ctest.*`为性能回修后的最终矩阵，CUDA增为24项；`followup-*`保留中间验证。`publish-retirement-memcheck.log`的非零结果是持续阻断完成后的16个隔离分配，不能写成零泄漏；普通Direct/device memcheck仍零错误。`performance-publish-paired.json`、`publish-paired-*`和`controlled-*.json/csv`对应最新同参数及固定同类CPU核补测。`performance-followup-paired.json`保留中间GPU到CPU额外拷贝回退，不删不利记录。`kvf_bench_after-pre-followup.txt`保留初轮after，`kvf_bench_after.txt`更新为最终实际运行输出。

源码清单的sha256对应实际本地测试文件；Git规范化换行后上传字节的sha256与blob SHA另存published字段。证据目录设置-text以保留CSV/patch原始字节。发布包含必要原有项目依赖，但不包含简历、模型及构建输出。
