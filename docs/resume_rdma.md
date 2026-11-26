# RDMA 简历定稿

跨实例重算慢→聚合收发与边收边H2D→TTFT降36.9%，KV就绪吞吐达链路上限62.5%；双netns+SIW软件RDMA隔离，超时断链回退且Token一致。

共 81 字符。评测为 Qwen2.5-3B BF16、16K 输入 / 8K 共享、16 输出；RTX 4060 Laptop 8 GiB。双 netns + 带命名空间补丁的 SIW 软件 iWARP；不冒称 RXE Soft-RoCE 或物理 HCA。TTFT 5.568→3.511 s；62.5% 的分母为 288 MiB ib_write_bw 实测锚点，分子为包含控制/CRC/H2D 的 KV 就绪吞吐。

[完整实测对账](RDMA-CROSS-INSTANCE-AUDIT.md)
