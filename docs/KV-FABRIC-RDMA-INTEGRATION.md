# KV-Fabric RDMA rsockets 集成

Worker默认使用TCP。`--kv-transport rdma`选择rsockets数据面；默认允许连接失败后的TCP回退，`--no-kv-rdma-fallback`要求严格RDMA。Directory控制连接继续使用现有控制协议。

发布者与消费者应使用可路由的RDMA接口IPv4地址。TCP和rsockets监听按各自fd类型等待；接收锁不跨poll或休眠。逐层只读传输可在原deadline内通过TCP从头重取，部分批次失败不提交不完整页，CRC与lease检查保持。

实际部署需要可用HCA或SoftRoCE、相应驱动和用户态库。链接期故障替身只验证错误处理，不代表真实RDMA性能或跨机验收；本次硬件验证状态见[验证摘要](KV-FABRIC-VALIDATION-REPORT.md)。


## 当前严格SoftRoCE复验

Linux RXE已在独立临时veth上运行。当前修复代码的CPU/CUDA各30轮262144字节传输均匹配，禁用TCP回退并核对RDMA_RSOCKET。新测试入口为tests/worker/test_rdma_roundtrip.py。临时网络资源在测试后删除；未修改现有上网接口。公开测量数据见docs/benchmarks/published-validation。
