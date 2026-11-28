# KV Fabric 项目交付

KV-Fabric 是 C11 分布式 KV Cache 项目，覆盖目录与租约、CPU/GPU 页生命周期、TCP/RDMA 传输、CPU/GPU/NVMe 分层存储，以及 Nano-VLLM 与 vLLM V1 Connector。源代码、回归测试、运行脚本、脱敏测量数据与公开简历均包含在仓库中。

## 项目入口

| 目录 | 内容 |
|---|---|
| `src/control`、`include/kvfabric` | Wire V3、目录、副本、租约、C SDK |
| `src/worker`、`include/miniinfer` | TLSF、Paged KV、迁移、Direct I/O、io_uring、CUDA、RDMA |
| `integrations/vllm`、`src/integration` | Scheduler/Worker Connector、C ABI、GPU Gather/Scatter |
| `src/nanovllm` | C11 推理、Prefix Cache、Remote KV 集成 |
| `src/replication`、`src/store` | Legacy KVStore 与认证复制兼容模块 |
| `tests` | 功能、并发、故障注入、真实模型回归 |
| `scripts`、`tools` | 基准、Nsight 归因、完整验证及简历渲染 |
| `docs/benchmarks/published-validation` | 移除主机标识后的测量数据与口径 |
| `docs/resume_public` | 脱敏 PDF、Word 简历；私人联系方式不公开 |

计算侧 nano-vLLM-C 是[独立项目](https://github.com/sahal-colder/nanovllm/tree/codex/paged-decode-cuda)，其 SGEMV、RMSNorm、Paged Attention 与完整 Decode 的指标有独立来源；不将两个项目描述为已经完成同一模型的端到端对接。

## 构建与运行

基础依赖为 C11 编译器、CMake、pthread 和 OpenSSL Crypto。GPU 路径需要 CUDA Toolkit；Direct uring 需要 liburing >=2.3；RDMA 需要 rdma-core、可用 HCA 或 SoftRoCE 及 IPv4 路由。

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j8
ctest --test-dir build-release --output-on-failure

cmake -S . -B build-cuda-release -DCMAKE_BUILD_TYPE=Release \
  -DMINIINFER_WITH_CUDA=ON -DNANO_WITH_CUDA=ON \
  -DMINIINFER_WITH_URING=ON -DMINIINFER_BUILD_VLLM_BRIDGE=ON \
  -DMINIINFER_VLLM_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-cuda-release -j6
ctest --test-dir build-cuda-release --output-on-failure
```

架构 89 对应验证用 GPU，应按目标设备设置。目录与 Worker 的运行方式见 [README](../README.md)、[数据面部署](kvfabric-data-plane.md)及 [vLLM 集成](../integrations/vllm/README.md)。

GitHub Actions自动运行CPU Release、ASan/UBSan/LSan及Legacy/Connector方法回归。GPU、SoftRoCE、NVMe和真实vLLM使用下述硬件回归，不冒充托管CPU runner的CI结果。

## 完整 vLLM 与物理 NVMe 回归

验证环境使用 vLLM 0.31.0、Qwen2.5-3B-Instruct BF16、单张 RTX 4060 Laptop。`VLLM_BATCH_INVARIANT=1` 是严格冷热 Token ID 对照的共同条件；默认 BF16 冷热数值差异在纯 vLLM APC 同样复现，不能通过删除断言规避。

在独立 vLLM Python 环境下，设置本机模型目录：

```bash
export MODEL_DIR=/path/to/Qwen2.5-3B-Instruct
export VLLM_BATCH_INVARIANT=1
python tests/vllm/test_connector_roundtrip.py \
  --model "$MODEL_DIR" \
  --library build-cuda-release/src/integration/libkvf_vllm.so \
  --directory build-cuda-release/bin/kvfabricd --require-uring-fixed \
  --out /path/to/new-connector-results
```

该测试启动真实服务，覆盖 CPU/NVMe 冷算、热命中、部分命中、超时回退及 GPU 层块核验。NVMe 数据文件应放在 ext4/NVMe 上，不写裸块设备。

```bash
mkdir /path/to/new-nvme-results
build-cuda-release/bin/kvf_nvme_recovery_bench \
  /path/to/new-nvme-results direct-uring-fixed
```

完整 27 格历史矩阵及 Nsight 数据已完成；新机器可先准备 CUDA uring 与 CUDA POSIX 两套构建，再用以下流程创建全新本地归档：

```bash
python scripts/run_native_linux_final.py prepare --archive /path/to/new-run \
  --build build-cuda-release --build-posix build-cuda-posix --model "$MODEL_DIR"
python scripts/run_native_linux_final.py all --archive /path/to/new-run
python scripts/summarize_native_linux_final.py --archive /path/to/new-run
```

所有阶段使用同一个独立 vLLM 环境。`profile` 阶段需要安装 Nsight Systems；Nsight Compute 的硬件计数权限单独配置。输出包含本机信息，发布前应脱敏。

## 严格 RDMA 验证

预先配置可用 HCA 或独立 SoftRoCE 测试接口。下面的测试不会提权或改变网络配置：

```bash
python tests/worker/test_rdma_roundtrip.py --build build-cuda-release \
  --address RDMA_INTERFACE_IPV4 --devices cpu cuda --rounds 30 \
  --out /path/to/new-rdma-results
```

每轮必须观测到 `RDMA_RSOCKET`，禁止 TCP 回退，并逐字节比较载荷。SoftRoCE 证明真实 RDMA 协议栈的功能，不等同物理 HCA/跨机吞吐或 GPUDirect RDMA。历史断连、超时与重新建连证据另有明确范围。

## 验收与兼容性

19项审计缺陷已实施安全修复，Release、CUDA、Sanitizer 及并发故障矩阵见[验证报告](KV-FABRIC-VALIDATION-REPORT.md)。未知 DMA 完成时保留资源并使实例安全失败，需退出进程回收，不称为永久故障也零资源保留。Wire V3 字段保持；Legacy 复制要求双方升级认证 V2，远程 SAVE/LOAD 默认拒绝。

已有实测覆盖 SoftRoCE、物理 NVMe 文件回源和完整 vLLM。2026-11-28 补齐了三星物理 NVMe 的 SPDK/VFIO 接管、裸盘读写、CPU/GPU 分层回源、推理缓存恢复与驱动切回验证，见[SPDK 物理盘验收](SPDK-NVME-VALIDATION.md)。尚未执行的范围是物理 HCA 跨机/GPUDirect、运行中物理 NVMe 移除、在线 GPU 故障恢复，以及生产级多卡性能 SLA。

## 简历

[公开 Markdown](resume_ai_infra.public.md)、[公开 PDF](resume_public/AI_Infra_Resume.pdf)及[公开 Word](resume_public/AI_Infra_Resume.docx)使用相同项目事实。姓名、联系方式、私人备份和本机提权凭据不在公开版本中。完整本地简历独立交付。
