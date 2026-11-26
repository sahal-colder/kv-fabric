# KV-Fabric 审计修复摘要

本次发布包含修复代码、回归测试及必要的既有RDMA/vLLM集成依赖。完整本地账本和原始证据未上传；本文件为不含主机身份、绝对路径、凭据或原始日志的公开摘要。

19项中，5项在修改前代码实际复现，14项由调用链确认。19项均实施修复；其中租约/CUDA异常完成采用明确的安全失败策略：无法确认DMA结束时保留所有者和内存，拒绝实例继续复用，需进程退出回收。不能把该策略称为永久故障也零资源保留。

| # | 问题 | 修复与回归 |
|---|---|---|
| 1 | Batch grant容量 | 按字段推导31916字节；8页×128层、最长合法名称及ticket真实RPC |
| 2 | NULL边界 | 公开owner、Directory、client与codec拒绝NULL |
| 3 | Hide/republish/旧失效 | 发布代数隔离；4线程各万轮，旧ticket失效且新页保留 |
| 4 | 跨批租约期限 | FINISH后本地快照不依赖已释放首批lease；跨TTL回归 |
| 5 | RDMA故障与TCP回退 | 逐层从头重取，保留CRC/lease；部分batch安全失败 |
| 6 | Accept循环 | 按fd类型poll；accept锁外等待；抑制SIGPIPE |
| 7 | io_uring安全 | SQE/CQE/submit/EINTR有界处理；自有DMA缓冲及未知完成隔离 |
| 8 | Direct I/O | depth/对齐/大小边界；NONE与READ_BACK载荷校验语义区分 |
| 9 | Lease异常收尾 | writer/count/栈lease及page引用正常收尾；Storage保留未知DMA物理所有权 |
| 10 | 迁移原子性 | 复制完成前保留原slot/replica；原子提交、回滚及cleanup重试 |
| 11 | TLSF | 向上取整与FL/SL bitmap O(1)查找；同桶、随机、合并及对齐 |
| 12 | CUDA异步生命周期 | 后端自有Host缓冲；迁移保留两端；Backend/Graph持池引用；64并发快照 |
| 13 | SPDK关闭 | owner-thread poller完成channel/descriptor/DMA收尾，消除错误回退与忙等 |
| 14 | vLLM Connector | 指标锁、Future错误、partial block/prior和stride预算 |
| 15 | 远程SAVE/LOAD | 拒绝未认证远程文件管理，内部持久化API保留 |
| 16 | 复制认证 | V2双nonce、角色HMAC-SHA256、防重放、握手deadline及槽位限制 |
| 17 | RESP缓冲 | 显式容量/所有权、溢出检查及大Value响应 |
| 18 | 多键原子性 | 四引擎先准备再无分配提交；穷举准备分配失败点 |
| 19 | RDMA等待 | 单调deadline及zero-time rpoll；链接期替身超时回归 |

Wire V3字段不变。Legacy复制握手要求两端升级V2、配置独立密钥与复制端口，不接受不安全V1降级；远程SAVE/LOAD权限有意收紧。外部C调用者须重新编译含新增配置字段的接口。

故障注入仅使用测试目标宏或链接器wrap；生产构建不含注入入口。原本地工作区及暂存区保留，没有将简历、模型、构建产物或原始审计日志纳入发布。

硬件依赖与剩余生产限制见[验证摘要](KV-FABRIC-VALIDATION-REPORT.md)。


## 硬件与完整集成更新

#5/#6/#19已有SoftRoCE真实协议栈CPU/CUDA各30轮严格传输复验；#8已有物理NVMe文件路径复验；#14已在独立vLLM 0.31.0环境通过真实模型的CPU/NVMe完整回归。之前笼统的BLOCKED不再作为当前结论。物理HCA跨机/GPUDirect、SPDK裸设备接管、设备移除/在线恢复与多卡SLA保留为未执行范围。
