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

以下仍未验收，不计PASS：真实HCA/SoftRoCE与多节点网络、物理SPDK NVMe及设备移除/在线恢复、完整vLLM模型调度集成、生产多卡/长上下文模型。SPDK结果仅针对malloc bdev；RDMA链接替身不能替代硬件测试。

其他生产差距包含永久故障资源的进程级回收、原有cache锁跨部分迁移I/O、Legacy多记录WAL缺少崩溃事务，以及目录HA、TLS和全局背压治理。本次为经过验证的代码修复交付，不是完整生产认证。

复验：默认Release使用项目标准CMake/CTest流程；Sanitizer配置使用KVF_SANITIZE=ON；GPU/uring/SPDK按相应项目开关构建。新增测试均已注册在CMake/Make中。
