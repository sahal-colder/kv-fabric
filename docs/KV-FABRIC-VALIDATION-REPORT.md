# KV-Fabric 验证摘要

本文件仅发布非识别性的验证结论。完整命令、环境探测、原始CSV、失败历史和哈希账本保留在本地，没有上传原始审计证据。

| 最终配置 | 结果 |
|---|---:|
| Release | 18/18 PASS |
| CUDA + io_uring + bridge | 24/24 PASS |
| ASan + UBSan + LSan | 19/19 PASS |
| TSan | 19/19 PASS |
| SPDK malloc bdev | 20/20 PASS |
| 纯TCP | 19/19 PASS |
| Storage-only | 10/10 PASS |
| Python方法及服务安全 | 15/15 PASS |
| Legacy独立单元入口 | 7/7 PASS |
| 独立发布源码快照 | Release 18/18、Legacy单元及Python 15/15 PASS |

并发回归包含同前缀hide/republish/旧代invalidate/lookup四线程各万轮，Storage四线程读写迁移，TLSF三十万轮随机，以及64线程同时GPU快照。核心故障退休、Graph及Storage用例执行重复回归。未删除断言、关闭CRC或放松租约校验。

故障测试覆盖分配失败、迁移复制/清理失败与空间不足、uring提交/等待/CQE/取消失败及连续EINTR、RDMA链接替身的连接/发送/接收/reset/partial、TCP超时、Directory重启、跨批Lease过期、CUDA完成错误与复制认证防重放。

普通Direct CUDA及四线程Stream/Event memcheck为0 errors / 0 bytes leaked。持续阻断完成的六种故障用例，功能断言通过，但memcheck返回99，报告16个保留分配、共1671158字节；报告均为Leaked allocation，未观察到Invalid/Uninitialized访问。该结果标为EXPECTED RETENTION，**不标作零泄漏PASS**。未知完成的内存不能回收或复用，必须退出进程；外部提交到同一流的其他资源由调用者自行隔离。

GPU后端额外预留64×align_up(block_bytes,4096)字节的Host传输缓冲，另有至多4095字节基址对齐开销，不计作GPU显存。迁移已持有源/目标所有权，直接搬运并在未知完成时保留两端，避免重复Host拷贝。

性能使用同参数交替比较，并保留不利样本。已消除迁移额外拷贝导致的确定性回退；最终GPU→CPU、SSD→GPU分别为-0.10%、-1.19%。固定同类CPU核的TLSF、Page及Directory补测在3%以内；未固定核的短Directory RPC仍有超过5%变化。不能据此签署所有部署场景的5%性能SLA。

以下仍未验收，不计PASS：物理HCA跨机/GPUDirect、SPDK裸NVMe设备接管及设备移除/在线恢复、生产多卡模型。已完成的SoftRoCE、Direct NVMe及完整vLLM单机集成见下节。SPDK结果仅针对malloc bdev；RDMA链接替身不能替代硬件测试。

其他生产差距包含永久故障资源的进程级回收、原有cache锁跨部分迁移I/O、Legacy多记录WAL缺少崩溃事务，以及目录HA、TLS和全局背压治理。本次为经过验证的代码修复交付，不是完整生产认证。

复验：默认Release使用项目标准CMake/CTest流程；Sanitizer配置使用KVF_SANITIZE=ON；GPU/uring/SPDK按相应项目开关构建。新增测试均已注册在CMake/Make中。


## 完整集成与证据复核更新

此前把默认Python缺少vLLM、未临时配置RXE解释为项目未完成，已纠正。独立vLLM 0.31.0环境与既有27格405请求、345条严格Token对照、物理NVMe和完整Nsight/NCU归档存在；历史293项原始文件哈希一致，4项后续简历更新单独记录。历史性能属于相应日期/源码，不冒称修复后的新测。

当前修复代码已通过Qwen2.5-3B BF16真实服务的CPU/NVMe冷算、热命中、部分命中、超时回退8类回归，3384次GPU层块核验；共同条件VLLM_BATCH_INVARIANT=1。未设置该条件的首轮严格输出不一致保留在本地，最终没有弱化Token断言。

当前内核SoftRoCE在独立临时veth上实际运行，CPU/CUDA各30轮严格RDMA往返均逐字节一致，实际transport=RDMA_RSOCKET；可移植测试再次通过60轮。物理NVMe/ext4文件上的72MiB回源15样本P50为58.465846ms、有效吞吐1.202625GiB/s，保留CRC与逐字节核验。没有写裸块设备，也不把RXE称作物理HCA或GPUDirect。

计算侧nano-vLLM-C的43个源码哈希一致，8/8 CTest复验通过；其算子与Decode结果属于独立项目。完整交付步骤、公开数据与脱敏简历见[项目交付](PROJECT-DELIVERY.md)。


## 修复后完整矩阵补测

4K/8K/16K × 重算/Native/CPU/NVMe共12格、180请求完成，135条非参考请求逐Token对照通过。每格预热3次、15样本、输出16Token、50%共享前缀；两种KV-Fabric路径实测50%外部命中，加载失败和写入错误均为0。中断的NVMe模式已完整重跑，未混入其未完成样本。

| 模式 | 16K TTFT P50 ms | P95 ms | P99 ms |
|---|---:|---:|---:|
| recompute | 5615.154 | 5618.753 | 5618.753 |
| native | 3207.027 | 3218.541 | 3218.541 |
| kvfabric-cpu | 3273.322 | 3282.575 | 3282.575 |
| kvfabric-disk | 5057.472 | 5355.138 | 5355.138 |

完整数据、实际命中层与save_skipped计数见公开验证目录。n=15的P95/P99为样本最大值。历史及新矩阵不混合统计，也不据不同日期数据签署严格全场景5%SLA。
