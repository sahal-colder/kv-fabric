# KV-Fabric 审计修复账本

> SPDK 更新（2026-11-28）：物理 NVMe 接管、裸盘读写及 CPU/GPU 分层回源已通过，详见[物理盘验收记录](SPDK-NVME-VALIDATION.md)。历史“仅 malloc bdev”的范围已扩展，运行中物理拔盘与在线恢复仍未验证。

> 当前交付状态更新（2026-11-26）：已核验既有完整 vLLM/NVMe/RDMA 实测证据，并对修复后的代码完成真实 vLLM 8类用例、3384次GPU层块核验及CPU/CUDA严格SoftRoCE各30轮。先前“当前默认Python无vLLM”和“未配置HCA”的探测只描述当时环境，不能据此否定已完成的独立环境和历史实测。原始阶段记录保留，当前结论以[完整交付指南](PROJECT-DELIVERY.md)与最新复验节为准。

本轮起始日期：2026-11-26（Australia/Perth）。最高事实基准是启动时的本地工作区；没有 reset、clean 或在原工作区切分支。首轮未提交；用户随后明确授权修复并上传GitHub，发布使用独立快照。

## 阶段状态

| 阶段 | 状态 | 证据 |
|---|---|---|
| Phase 0 快照、环境、功能与性能基线 | COMPLETE | `/tmp/kvf-audit/` |
| Phase 1 19 项只读复核 | COMPLETE | 下表与所有权说明 |
| Phase 2 核心修复 | COMPLETE（#9 后续安全隔离已补齐） | |
| Phase 3 扩展修复 | COMPLETE（#12 后续所有权修复见末节） | |
| Phase 4 受控清理 | COMPLETE | |
| Phase 5 验证矩阵 | COMPLETE（有限验收，见报告） | [验证报告](KV-FABRIC-VALIDATION-REPORT.md) |
| Phase 6 验收 | COMPLETE（文档交付；生产门槛未全部满足） | [验证报告](KV-FABRIC-VALIDATION-REPORT.md) |

## Phase 0：工作区保护

- 根目录 `/home/wty/Infra`；分支 `dev`；HEAD `55e314292bd6a054fa87ab3de6d07fc6bfe2ed66`。
- 启动时有 26 个已跟踪修改文件。修改文件与未跟踪文件合计 278 个，1,469,202,266 字节，逐文件 SHA-256 清单在 `/tmp/kvf-audit/preexisting-files.json`，原文归档在 `/tmp/kvf-audit/preexisting-files.tar`。
- 原始差异 `/tmp/kvf_pre_audit.patch`；暂存区差异 `/tmp/kvf_pre_audit_index.patch`；未跟踪列表 `/tmp/kvf_untracked.txt`；已跟踪列表 `/tmp/kvf_tracked.txt`。
- 没有发现适用的 AGENTS.md。用户既有文件不覆盖，本轮新增测试与变更以启动清单区分。

## Phase 0：环境与基线进展

- Linux 6.8.0-138-generic；GCC 11.4.0；CMake 3.22.1；i7-13700H，20 个逻辑 CPU，约 15 GiB RAM。完整环境：`/tmp/kvf-audit/environment.txt`。
- CUDA Toolkit 13.4 可找到，但 `nvidia-smi` 无法连接驱动。CUDA 实机测试暂列 BLOCKED。
- liburing 2.9；内核 `io_uring_disabled=0`，实际 ring 初始化仍需探测。
- librdmacm 1.3.39.0、libibverbs 1.14.39.0；没有 `/sys/class/infiniband`，RDMA 实机传输暂列 BLOCKED。
- pkg-config 报 SPDK 19.1；实际构建/设备可用性待验证。
- Python 3.12.15、pytest 9.1.1；当前 Python 环境无 vLLM；torch 2.11.0+cu128。
- 默认 Release 配置与构建成功。受限沙箱中 CTest 为 8/14 通过，6 项网络测试失败；独立 socket 探测返回 EPERM，已开始在允许 loopback 的执行环境重跑。不能把沙箱错误当作源码缺陷，也不能删测试。
- 日志：`baseline-configure.log`、`baseline-build.log`、`baseline-ctest.log`、`baseline-ctest.xml`（均在 `/tmp/kvf-audit`）。性能基线尚在执行，完成前不修改业务源码。


## Phase 0 完成证据（后续修正优先于早期探测）

- 沙箱外默认 Release **14/14 PASS**（15.07 s），CUDA + Nano CUDA + uring + vLLM bridge 构建 **17/17 PASS**（37.34 s）。未发现原有适用 CTest 失败。`baseline-ctest-local.*` 与 `baseline-cuda-ctest.*` 保存明细。
- GPU 实际为 RTX 4060 Laptop 8 GiB，驱动 615.71.09；初次不可见为沙箱限制。宿主 `io_uring_queue_init=0`。GPU 与 uring 不能笼统标 BLOCKED。
- 宿主 ASan/UBSan/LSan 探针均成功；TSan 探针以 `unexpected memory mapping` 退出 66，运行验证 BLOCKED。
- 基线命令、参数、原始输出：`/tmp/kvf_bench_before.txt`；固定微基准源码 `/tmp/kvf-audit/micro.c`，重跑入口 `/tmp/kvf-audit/run-bench.py`；CSV 在 `before/`、`before-cuda/`。GPU 基准：`kvf_cuda_bench all`（128 页、36 层、72 MiB、种子 734）与 `kvf_nvme_recovery_bench PRIVATE_DIR direct-uring-fixed`（144 块、512 KiB、3 预热、15 样本，逐字节校验）。GPU 源码改动前已执行。
- 微基准为每组 7 轮：TLSF 2000×128 alloc/free（64 B 对齐，8 MiB 池），Page 100000 次 create/append/destroy，Directory 1000 次 stats RPC；SSD/Remote 使用仓库基准 64 KiB、4 客户端、4 预热、3×16 样本。碎片项仅测完全合并后最大块可分配，不代表随机负载碎片率。
- Legacy `make -j8 BUILD_DIR=build-legacy-baseline` 构建成功；其独立测试尚需执行。

## Phase 1：锁序与所有权（修改前）

- Remote：`updates -> exports_lock -> Trie mutex -> Storage mutex -> Backend mutex -> Pool mutex`。updates 只串行控制面变更；允许其跨 RPC，但严禁 exports/Trie/Storage 跨 RPC、磁盘 I/O 或 CUDA 同步。Serving 仅在 exports_lock 内克隆 page_ref，锁外读取/传输；最后校验 Directory ticket。Nano 的 `r->lock -> exports_lock` 不反向获取 r->lock。旧异步失效只有 hash，无发布代数。
- Storage：记录用 `busy`、writer、lease_count 钉住；迁移锁内设 MOVING，锁外搬运，锁内提交。replica 必须保持到目标完成，所有字段提交均在 Storage mutex 下。Page lease 额外持一页引用，底层 lease 未安全结束时不能放掉页。
- I/O：Segment 借用 -> Ring mutex；正常 CQE 消费后才归还。现有 wait 硬错误分支无限 peek/yield；必须有明确退出且不可释放仍在途的用户缓冲。故障测试只能链接期 wrap/测试编译入口。
- Legacy：Store rwlock -> Persistence/Replication mutex；网络写在 Store 锁外。现有多键只有可见性互斥，没有分配失败的事务回滚。快照已有 staged store + swap，可复用现有交换能力，但不可大规模重写引擎。

## Phase 1：19 项逐项复核（初始结论，测试后再升级为 REPRODUCED）

行号引用启动时源码；最终变更位置将在验收时补充。CONFIRMED 表示调用链成立，不等于已运行故障用例。

| # | 结论 | 实际位置/函数与触发影响 | 最小修复与回归计划 |
|---|---|---|---|
| 1 | CONFIRMED | `include/kvfabric/wire.h:13`、`src/control/{wire.c:184,control.c:165,client.c:154,net.c:131}`。grant 为 manifest+endpoint+ticket+ttl。实际固定 405 B（KVF_WORKER_NAME=48），旧 384 B，8 页少 168 B；Writer 安全失败但最大合法批次不可用。 | 按序列化字段定义常量，不改 Wire V3 字段；最大 8×128 层、47 字符 owner、ticket 编码/真实 RPC 测试与少 1 B 边界。 |
| 2 | CONFIRMED | `src/control/identity.c:143` owner_valid 解引用 NULL；`directory.c:250` worker_status 缺 d/name/out/online 防御，`client.c:36` 同样。内部 find_worker 由公开边界保证。 | 公开指针边界拒绝非法参数，codec 标记 failed；NULL 回归。 |
| 3 | CONFIRMED | `src/worker/remote/cache.c:11,185,194` export_page 对 hidden 返回 OK；hide/invalidate 跳过 order=0；`src/nanovllm/nano_remote_kv.c:331,519` 后台任务只带 hash。新旧同前缀任务混淆。 | 增加内部发布代数与带代数失效接口，旧 API 保留；隐藏页不再当活页，先退休后新发布；4 线程循环测试、旧 ticket 拒绝与新代保留。 |
| 4 | CONFIRMED | `cache.c:422-531` result.valid_until_ms 累计所有批次最小值，FINISH 后继续判首批过期；Nano 消费同一结果字段。 | 每批独立 expiry 与请求 deadline，成功 FINISH 后本地完整快照不再依赖已释放 lease；延迟第二批测试。 |
| 5 | CONFIRMED（部分已加固） | `transfer.c:336-530` batch 有建连回退、复用连接一次重连，部分包失败不提交；逐层 import 已连 RDMA I/O 失败直接退出。 | 同一只读层在原 deadline 内允许一次 TCP 完整重取，错误连接逐出；wrap connect/send/receive/reset/partial，CRC 与 lease 不减弱。硬件 RDMA BLOCKED。 |
| 6 | CONFIRMED（部分已加固） | `worker.c:31-59` accept_lock 内 poll/1 ms sleep；混合 fd 以 transport_mode 调 rpoll；remote_sock 已有 zero-time rpoll 规避旧库挂起；统计大多 atomic。 | 锁外按类型 poll，锁内非阻塞 accept；TCP/RDMA 独立就绪检查与退出有界；验证端口、SIGPIPE 和重复启动关闭。 |
| 7 | CONFIRMED | `uring_io.c:21,60-100` SQE 无判空、CQE 未初始化、submit 异常后不排除在途、wait 错误无限 yield。 | 不向内核提交调用者短生命周期资源；有界恢复/取消，无法确认完成则隔离资源；链接期注入 SQE/submit/wait/CQE/EINTR。 |
| 8 | CONFIRMED | `direct_io.c:35-88,128-207` rings[64] 缺本地 depth 边界（公开 backend 已有部分校验）；valid_record 始终算 payload CRC。 | depth=0/1/64/65、本地 stride/offset/size 检查；NONE 跳过 payload CRC，头身份/CRC 保留；损坏头/载荷测试。 |
| 9 | CONFIRMED | `tiered_storage.c:249-286` wait 失败 lease.active/writer/count 保留；调用者通常栈上丢 lease；`paged_kv.c:313` 因 active 不释放页。 | 明确失败 quarantine 与可重试完成，不能简单清 active 后释放 GPU 内存；wrap stream wait 测试提交/中止/重复收尾/页引用。 |
| 10 | CONFIRMED | `tiered_storage.c:371-429` reserve 后复制前释放 r->replica，且在 Storage 锁外写字段；busy 排斥普通 acquire，但 metadata/cleanup 路径需统一。 | 原副本保留，复制成功后原子切换，旧槽锁外清理、失败保留原位；注入 reserve/copy/release 失败及并发读迁移。 |
| 11 | CONFIRMED | `allocator.c:252` 同桶只看 head，再跳下一桶，结果取决于链表顺序。 | 按 TLSF 向上取整映射保证选中桶容量、FL/SL bitmap O(1)，不扫描链；同桶/边界/随机/合并/对齐/压力。记录量化带来的有界内部碎片。 |
| 12 | CONFIRMED（部分安全路径存在） | `device_cuda.cu:38-158` out NULL 解引用、device<0 destroy 仍调 CUDA、零长度 NULL copy 未明确；`kv_cache.c:223` async 使用 Storage ticket。GPU 数据当前未发现 host 解引用。 | 统一 CPU/CUDA 边界与零字节语义；stream/event 定向同步，故障不提前回收；GPU 回归、Graph 既有测试、边界测试。 |
| 13 | CONFIRMED | `spdk_bridge.c:274` send_msg 非 ENOMEM 失败时绕过 stop(d) 直接 app_stop，channel/desc/DMA 的 owner-thread 清理被跳过；ENOMEM 无限重试。 | 用 SPDK reactor 自身 poller 处理关闭请求，要求指定线程执行 stop；初始化失败/关闭测试，SDK 构建/运行条件单独核验。 |
| 14 | CONFIRMED | `integrations/vllm/kvfabric_vllm/connector.py:128,150-175,328,372` 指标无锁、lookup/save Future 异常外逃、partial block 超算、prior None；`src/integration/vllm_bridge.c:142` CPU budget 用 payload 而非物理对齐 stride。 | 指标锁、明确异常结果、按覆盖完整块计算可用 token、缺 prior 安全恢复、预算按 backend stride、NULL 检查；Python stub 单测与真实 C bridge 测试。当前 Python 无 vLLM，完整集成 BLOCKED。 |
| 15 | CONFIRMED / PATH_CHANGED | 实际 `src/protocol/kvs_text.c:31`、`src/redis/kvs_redis.c:472`，配置无认证，远程可给任意文件名。 | 默认禁用远程 SAVE/LOAD（内部持久化 API 保留），避免路径穿越与 symlink/TOCTOU；协议回归确认无文件产生。记录兼容性收紧。 |
| 16 | CONFIRMED / PATH_CHANGED | `src/replication/kvs_master.c:137,278`、`kvs_slave.c:91` 仅固定公开 magic 握手即可触发快照；64 peers 有队列/超时但无鉴权。 | 增加配置共享认证及有限握手，未认证不生成快照/占槽；拒绝无凭据启用复制；认证线格式兼容影响必须记录并保留安全失败语义。 |
| 17 | CONFIRMED / PATH_CHANGED | `src/protocol/kvs_resp.c:131` used+length+2 溢出；`src/network/kvs_connection.c:17` next*=2 溢出；protocol.c 栈上 1024 回复会拒绝大 GET。 | 保留原有定长 API、增加带容量回复路径，网络层按请求结果安全扩容；整数边界与大 Value 回归。 |
| 18 | CONFIRMED / PATH_CHANGED | `src/redis/kvs_redis.c:308-369` 多键循环先写再分配，Store 锁不保证失败回滚。 | 基于现有 store staging/swap 先完成可失败步骤，再整体提交及日志/复制；注入各分配点失败，含重复 key。 |
| 19 | CONFIRMED / PATH_CHANGED | `src/replication/kvs_transport.c:62` 对 rsocket 使用有超时 rpoll，未采用 remote_sock 已有规避；EINTR 重新给全 timeout。 | monotonic deadline + rpoll(...,0) + 有界短休眠，TCP poll 保持；fake rpoll 单测超时，真实 RDMA BLOCKED。 |

初始统计：19/19 有至少一个成立的子问题，0 项可整体归为 STALE/FALSE_POSITIVE；5/6/12 有既有加固，不能重复宣称全新修复。PATH_CHANGED 为附加定位标签，不能与缺陷数重复相加。

### Phase 2A：Control Plane 完成

- 最大编码：owner=2+47+16=65；identity=5×32+3×4+5×2=182；layer=28；manifest=64+32+65+182+8+128×28=3935；ticket=44；grant=3935+6+44+4=3989；batch=4+8×3989=31916。原容量 31748，差 168 B。发送/接收/Frame 上限均复用同一常量，Wire 字段与版本不变。
- 新增 `test_max_batch_and_null`，真实 Directory RPC 编码/发送/接收/解码 8 页×128 层、47 字符 name；独立 codec 写 UINT64/UINT32 最大 ticket，精确容量与少一字节失败且哨兵不变。
- owner_valid/owner_equal、Directory 入口、worker/identity/manifest codec、client 状态查询等公开边界加固。没有在 find_worker 堆叠冗余判空。
- `cmake --build build-release -j8` 成功；全量 CTest **14/14 PASS**（15.22 s）。证据 `control-ctest.*`。ASan 于 Phase 5 集中再验。

### Phase 2B：Remote 完成首轮回归

- hide 适用于全部发布（包含旧 order=0 入口），返回单调发布代数；新增两个附加 API 供异步调用捕获/校验代数，原 API 保留。Nano 退休任务带 revision；无对应导出时不提交无代数的延迟删除。
- hidden 页发布前先在 exports_lock 外完成旧 manifest 的 Directory invalidation，完成后释放旧页引用；失败保留 RETIRING。心跳重试与引用释放一致。
- 每批 FINISH 成功表示本地快照已独立拥有，清除已释放 lease 的 expiry，Request Deadline 仍严格检查。
- Accept 按 TCP/RDMA fd 类型分别 poll，accept_lock 只覆盖非阻塞 accept，不覆盖 poll/sleep；RDMA send 添加 MSG_NOSIGNAL。
- 逐层只读导入在同一 deadline 内支持 RDMA I/O 失败后 TCP 从头重取，旧连接逐出。Batch 部分 body 仍安全失败、不提交；没有把部分数据当成功。
- 新增 `remote_audit`：4 线程×10000 轮 hide/republish/旧代 invalidate/lookup，新代 block ID 必须保留，旧 ticket 必须失效，最终 leases=0；9 页跨两批，第二批前等待 300 ms（TTL 200 ms），整体成功。
- rsocket 链接期替身在真实 TCP 上注入 connect/send/recv/reset/partial，逐层回退成功且逐字节正确；batch partial 返回 IO 且只收到 17 B。该项是确定性故障测试，不等同 HCA 实测。
- 全量 Release CTest **15/15 PASS**（17.95 s），`remote-ctest.*`。新增测试初版误用 legacy resolve（未进入 batch），已经改用实际 resolve_after，保留全部断言并真实通过。

### Phase 2C：Storage / TLSF 回归完成

- 迁移保留原 slot 与 replica 到复制成功；busy 期间在锁外 reserve/copy/release，锁内提交记录。内存层间迁移保留有效 SSD 副本，失败保留原位置；目标清理失败记录 cleanup 后可重试。
- Direct 内部 depth=0/65、未对齐 stride/offset、过大记录均拒绝；NONE 仍检查头 CRC/身份但不计算载荷 CRC；READ_BACK 校验完整载荷。公开 backend 原本已有 depth 校验，未将其归为全新修复。
- TLSF 将请求向上量化到 SL 边界，分割按量化大小；FL/SL bitmap 查找 O(1)，精确桶头匹配保留整池可分配行为。量化额外内部碎片小于该幂区间 1/32，另有 64 B 对齐开销。未使用链表扫描。
- io_uring 用 Ring 自有对齐缓冲（fixed 注册也由 Ring 持有）消除调用者栈 iovec/数据在不确定完成时失效的风险。SQE NULL、submit/wait EINTR/硬错误、CQE 初始化与结果校验均有退出路径；错误后同步取消。内核取消与 CQE 均无法证明结束时，destroy 明确隔离 Ring+buffer，禁止释放 DMA 内存；这种不可恢复内核错误可能保留资源，不能声称绝对无泄漏。
- 租约 wait 失败后用精确 Stream Event 确认完成，确认后中止写入并清 writer/count/active，page lease 正常释放页引用。**若 event 也不能确认完成，保留 active 及页引用，由调用者持有租约重试；这是一项安全隔离边界，永久设备故障的自动回收仍未解决，不能宣称完全修复 #9。** 不使用全局 device sync。
- 新增 `storage_audit`：复制失败保留副本、回滚 cleanup 失败/重试、wait 失败但 event 成功、wait+event 双失败时禁止 erase、恢复后中止、重复完成/中止、depth 0/1/64/65、载荷及头损坏与 NONE/READ_BACK。
- 新增 `uring_faults`（仅 uring 配置，测试专用编译缝隙与链接 wrap）：SQE NULL、submit error/EINTR、wait error/EINTR、CQE error/zero、已接受后返回 submit error。真实内核 ring 上通过。
- `pool` 新增原同桶误报场景与固定种子 30 万轮随机、逐字节 canary、64 B 对齐及完全合并校验。
- 默认 Release **16/16 PASS**（18.55 s）；CUDA+uring **20/20 PASS**（41.92 s）。证据 `storage-ctest.*`、`storage-cuda-ctest.*`。`build-baseline-cuda` 在此阶段已重建用于回归，原始基线数据/日志仍单独保存在 before-cuda 和 baseline-cuda-*；后续不可把其当前二进制当修改前二进制。

### Phase 3D：CUDA / SPDK / Connector 回归

- CUDA 与 CPU 设备层统一拒绝 NULL out、零字节分配及非零长度 NULL copy；零长度 copy 为无操作；device<0 stream/event destroy 不访问 CUDA。新增 `device_boundaries_cpu/cuda`：4 线程×100 次 H2D/D2H、独立 stream/event、逐字节校验。
- SPDK close 改为 atomic 关闭请求，由 owner thread 的定时 poller 执行 stop，不依赖 send_msg 成功，也不再从任意线程 app_stop。新增 `spdk_lifecycle`（真实 malloc bdev 写读并校验 app_stop 线程）与 `spdk_init_failure`。SDK 是 SPDK v27.01-pre 4828014 / DPDK 25.11.0（此前 pkg-config 19.1 是组件 ABI 版本）。2/2 PASS；初次默认池超出桥 256 MiB 内存预算，测试 JSON 已显式设置 iobuf/bdev 池规模，未弱化数据/线程断言。物理 NVMe SPDK 未验证。
- Connector metrics 互斥；lookup、receive、background save Future 异常记录并安全反馈；部分 block 返回覆盖范围减去已计算偏移；alloc 缺 prior 可恢复；C bridge CPU budget 按 backend 实际 stride 计算，io_mode NULL 防御；Python bridge 拒绝零形状。
- Python `tests/vllm/test_audit.py` **10/10 PASS**，使用实际方法 AST + 最小 base fixture，覆盖 4000 条并发 JSONL、partial block、lookup/receive/save 异常、prior 缺失。没有冒充真实 vLLM 集成。
- 真实 C bridge 8 页往返：3 层、layer_bytes=128 与 4100，3072 B / 98400 B 逐字节一致；命令 `/tmp/kvf-audit/bridge-test.py build-baseline-cuda`。当前 Python 同时缺 vLLM 与 nvtx，完整 scheduler/model 集成 BLOCKED。
- D 变更后默认 Release **17/17 PASS**（18.03 s），CUDA/uring **22/22 PASS**（42.03 s），日志 `extensions-ctest.*`、`extensions-cuda-ctest.*`。SPDK 日志 `spdk-ctest.*`。

### Phase 3E 兼容性论证（在修改 Legacy 前记录）

1. 当前 Legacy 无认证配置，无法安全保留客户端任意路径 SAVE/LOAD。将默认拒绝远程文件管理命令；进程内部快照/AOF API 和启动恢复保留。属于有意收紧远程管理权限，文档和协议测试必须同步，不能误报原行为仍兼容。
2. 复制 V1 公开 magic 不是认证，继续接受旧握手等价保留远程读全库漏洞。将要求配置独立随机共享密钥与专用复制端口，使用 V2 magic + 双 nonce 的 HMAC-SHA256 双向认证；数据帧布局保留。旧无凭据 peers 安全失败，部署需两端共同升级；不得自动降级到未认证 V1。密钥打印脱敏。专用 listener 避免在客户端事件循环执行耗时握手。
3. RESP 固定 1024 B API 作为兼容入口保留，网络执行增加明确容量/所有权的动态回复；不使用 TLS 隐藏状态，也不改变 RESP 字节格式。
4. 多键原子性优先复用现有 Store 类型与 staging/swap，先完成所有可失败分配再交换；日志与复制在成功提交后发出。持久化日志故障仍遵守原进程 fail-stop 策略。批量 WAL/跨崩溃事务若未实现必须单独列为生产差距。

### Phase 3E：Legacy 回归完成

- 远程文本/RESP SAVE/LOAD 一律拒绝，包含无参数、`..`、绝对路径、symlink；内部持久化 API 保留，实际短写/截断/CRC/恢复验证转到 `test_snapshot_resp` 的本地 API，未跳过断言。
- 新增 `kvs_auth.c`：V2 magic、server/client 32 B 随机 nonce、带角色标签的 HMAC-SHA256 双向证明、常数时间比较；握手共享绝对 deadline，最多 1 s；未认证不占 peer 槽/生成快照。启动强制 32..128 B secret 与独立复制端口，打印脱敏；数据帧未变，无不安全 V1 降级。OpenSSL libcrypto 成为 Legacy 构建依赖。
- RESP 增加显式 data/used/capacity/grow/owned buffer，普通小回复仍用协议栈缓冲，大回复先安全扩容再写；原固定容量 API 保留。bulk 与连接扩容做溢出检查；网络最大回复 64 MiB，超出返回错误。
- MSET/MSETNX 在只含本批键的私有索引完成可失败分配，再在 Store 写锁内无分配转交节点/值，不复制全库、不引入后端回调层。四引擎均覆盖重复 key 与失败原子性；DEL/UNLINK 在现有写锁内删除不需要分配，存在性检查与无分配删除原本已满足内存失败不部分提交。批量 WAL 的崩溃事务边界仍不是完整事务，列为生产差距。
- Legacy RDMA wait 改为 zero-time rpoll + monotonic deadline + 50 us 有界休眠，EINTR 不延长 deadline。真实 HCA 仍 BLOCKED，链接替身验证超时行为。
- 新增 `test_audit_atomic`：4 引擎分别穷举 6/8/8/11 个准备分配失败点，原值/不存在性/键数均不变；array 满容量回滚；32 KiB bulk、SIZE_MAX、连接长度溢出。
- 新增 `test_audit_auth`：双向认证与错密钥拒绝；RDMA=1 版本链接 wrap 验证 zero-time rpoll、EINTR 与 15 ms 截止（未冒充硬件 RDMA）。
- 新增 Python `test_audit_security.py` **5/5 PASS**：远程文件禁用、32 KiB GET、多键、既有六类客户端、70 次未认证握手无槽占用/无快照、错证明拒绝、合法主从快照+增量、缺凭据启动失败。
- Legacy 7 个单元入口均 PASS；既有 `test_sync_e2e.py`、`test_regressions.py`、`test_reliability.py`（reactor）均 PASS。旧共享 listener 与远程管理的预期已改为明确拒绝，持久化故障由本地 API 继续验证。日志 `legacy-unit.log`、`legacy-security.log`、`legacy-sync.log`、`legacy-regressions.log`、`legacy-reliability.log`。
- 原始 Legacy 单元基线补充：test_config/test_replication/test_snapshot_resp/test_allocator PASS；test_sync_wire 在沙箱 EPERM，宿主重跑 PASS，不算原有源码失败。

### Phase 4：受控清理完成

- 仅本轮模块：移除 export_page 已不再需要的 order 入参；移除认证后不再可达的 inline V1 ACK 状态；RESP 小回复保留单次格式化，只有扩容才重试格式化；修正新增测试的误导缩进。
- 补充验证覆盖：`storage_audit` 4 线程×10000 轮并发读/写/CPU-SSD 迁移并校验无撕裂、最终 lease_count=0；`allocator_faults` 穷举 24 个序号的元数据 malloc/calloc 失败并验证恢复后整池可分配。
- 清理后默认 Release **18/18 PASS**（18.54 s），CUDA/uring **23/23 PASS**（37.76 s），Python **15/15 PASS**、Legacy 7 个单元入口 PASS。证据 `cleanup-*`。
- 本轮构建日志无 warning/error；`git diff --check` 的三条 nano_cuda.cu CRLF 提示来自启动时用户已有改动，未动该文件。
- io_uring 的同步取消 API 要求 liburing >=2.3，CMake 现明确依赖；当前 2.9 满足。旧内核取消失败时仅能隔离未确认完成的自有资源，不能用释放缓冲掩盖问题。
- 为同轮性能复核在 `/tmp/kvf-audit/original-source` 用 HEAD+原始归档还原了启动业务源码，不写回工作区、不切分支；其 CUDA 基线二进制在独立 original-cuda 构建目录重建。

### Phase 5：验收进行中，性能问题已定位并回修

- 最终目录使用明确名称：`build-release`（CPU）、`build-cuda-release`（CUDA+uring+bridge）、`build-asan`（ASan+UBSan+LSan+uring）、`build-spdk`、`build-tcp`（RDMA=OFF）、`build-storage-only`。早期 build-baseline-cuda 仅作中途回归，不能误用为初始源码。
- 最大 Wire 用例进一步通过测试专用 Directory 翻译单元与 UUID 链接 wrap：真实 RPC 最后 8 个 fence 到 UINT64_MAX、slot 到当前最大合法 65535、directory/secret UUID 全 0xff；codec 额外检验解码与少一字节写失败。生产库不包含测试计数器入口。
- 冻结初始库真实复现 5 项：#1 最大 batch 返回 INTERNAL、#2 owner_valid(NULL) SIGSEGV、#3 hidden republish 返回 OK 但仍不可见、#4 第一批释放且过期后第二批导致整个 suffix STALE、#11 明明有 4288 B 空闲块却 ENOMEM。证据 `reproduced-before.log`、`reproduced-remote-before.log`，不是依据修后测试倒推“复现”。其余 14 项维持 CONFIRMED。
- 初始 Phase 0 vs 第一次 Phase 5 存在多项 >5% 波动；保存全部原始值。8 对 CPU/Remote、4 对 CUDA 的交替复测后，大多数差异缩小；SSD→GPU 仍约 +5.96%，定位为 Ring 自有 DMA buffer 引入的额外完整 memcpy。
- 性能回修：Direct I/O 直接借用 Ring 自有、4 KiB 对齐的 fixed buffer（GPU 场景使用 CUDA pinned allocation），保留 Segment 的 stream/event 槽及 Ring 对内核的完整生命周期所有权。正常 fixed I/O 不再往返拷贝。硬错误后不再向调用者提供该 Ring buffer；普通 uring 仍复制到自有 buffer，保护任意调用者数据寿命。
- 6 对交替恢复复测：GPU→Host -0.08%、Host→SSD -3.28%、SSD→GPU -1.01%、有效恢复吞吐 +1.02%（中位数）。先前约 6% 的确定性拷贝开销消失。数据见 `performance-final-recovery.json` 与 final-cuda-*/recovery.csv。
- 复核额外发现并修复迁移 cleanup 边界：新 SSD 居留提交后旧 SSD replica 释放失败时，下一次迁移不得覆盖其句柄；必须先 retry_cleanup。新增故障断言通过。
- CQE 持续 -EINTR 现在也有 16 次退出上限；测试覆盖持续中断以及直接访问固定注册 buffer 的写读、失败后禁止继续借用。
- 最终全量：Release 18/18、CUDA/uring 23/23、ASan/UBSan/LSan 19/19、SPDK 20/20、RDMA=OFF 19/19、Storage-only 10/10 均 PASS。NVIDIA memcheck 对 Direct CUDA 与四线程传输均 0 errors、0 bytes leaked。Python 15/15；Legacy 服务 Sanitizer 5/5，含 V2 proof 防重放。
- 不扩大通过结论：#9 永久设备故障后无法确认完成时仍 active；#12 的 snapshot/Direct/调用方栈租约在连续 CUDA 错误下未形成覆盖所有所有者的统一隔离和回收闭环。正常 GPU/Graph E2E 及单次故障不能证明永久故障路径安全。两项保留 PARTIAL，不签署“无已知异常生命周期风险”。
- 已观察到简历相关 6 个原有未跟踪文件在审计期间由外部更新（本轮工具未写这些路径）；保留当前内容，不还原，不纳入本轮代码修改统计。其他启动清单用逐文件 SHA-256 核验。
- 一次配对复测调用曾被自动审批服务额度错误拒绝，未执行；恢复后同一权限路径成功，未绕过审批。原始数据与中断历史均保留。

### Phase 5 补充：TSan 已恢复，测试竞态已修复

- `setarch $(uname -m) -R` 仅对测试进程关闭 ASLR，TSan 探针成功。此前 BLOCKED 是中间环境判断，最终不沿用。`build-tsan` 用 GCC `-fsanitize=thread -fno-pie` / `-no-pie` 构建。
- TSan 首轮 18/19，发现的是本轮 `remote_audit` 故障夹具在 worker 启动后修改 transport_mode，与 service thread 初始读取竞争。已使用不启动服务线程的独立 importer 承载链接故障注入，不修改运行中 worker 的 immutable 配置；没有抑制 TSan 报告或删除测试。原失败保存在 `tsan-first-run.*`。
- 修后全量 TSan **19/19 PASS**（47.44 s）。Legacy TSan 7 个单元入口、服务安全 5/5、真实网络并发/AOF/复制可靠性均 PASS。CUDA+TSan 组合未执行；GPU 由独立 CUDA E2E 与 NVIDIA memcheck 覆盖，不混写为 TSan GPU PASS。
- 最后补充 Direct I/O 文件隔离：硬 I/O 错误将整个 Direct 文件置为失败，后续 transfer/reserve 拒绝，避免无法确认旧请求完成时在其他 Ring 复用同一文件偏移。普通 CRC 不等于硬 I/O 错误，不触发该隔离；需关闭重建后端恢复。uring_faults 新增“完成等待失败 -> 后续 reserve=EIO”断言。
- Legacy `secret` 追加在配置结构体尾部，旧字段偏移保留，但结构体大小变化，外部 C 调用者必须重新编译；V2 握手迁移与远程 SAVE/LOAD 权限收紧已同步到当前部署文档及样例注释。未放置公共默认密钥。

## 当前逐项定位与处置状态（含后续修复）

复核标签与修复处置分开统计：REPRODUCED 5 项（#1/#2/#3/#4/#11），CONFIRMED 14 项；PATH_CHANGED 为附加定位标签。19 项均有成立的子问题，0 项整体 STALE/FALSE_POSITIVE。19 项均有安全修复与适用回归；#9/#12 使用安全失败并保留资源至进程退出的策略，真实设备丢失/恢复仍未验收。硬件 BLOCKED 不等于对应实现未修改，也不等于 PASS。

| # | 复核标签 | 当前实际函数范围 | 处置与验证 |
|---|---|---|---|
| 1 | REPRODUCED | `src/control/wire.c:191-204` (kvf_put_manifest)；`src/control/control.c:163-184` (control_request) | CLOSED：精确 31916 B 上限，真实最大 ticket RPC + ASan。 |
| 2 | REPRODUCED | `src/control/identity.c:143-156` (kvf_owner_valid)；`src/control/directory.c:257-271` (kvf_directory_worker_status) | CLOSED：公开边界 NULL 拒绝，原始 SIGSEGV 与修后断言。 |
| 3 | REPRODUCED | `src/worker/remote/cache.c:11-87` (export_page)；`src/worker/remote/cache.c:211-230` (mi_remote_invalidate_page_revision)；`src/nanovllm/nano_remote_kv.c:521-534` (nv_remote_evict) | CLOSED（本轮场景）：代数捕获/旧任务隔离、隐藏页替换、4 线程压测；独立故障 importer 保持 worker 配置不可变。 |
| 4 | REPRODUCED | `src/worker/remote/cache.c:451-558` (resolve_batch) | CLOSED：FINISH 后本地快照不依赖已释放 lease；跨批 TTL 复现/回归。 |
| 5 | CONFIRMED | `src/worker/remote/transfer.c:422-544` (mi_remote_import)；`src/worker/remote/transfer.c:361-420` (mi_remote_receive_batch) | CLOSED（可测试路径）：逐层 RDMA 故障后 TCP 从头取、CRC/lease 保留；batch partial 安全失败。真实 RDMA BLOCKED。 |
| 6 | CONFIRMED | `src/worker/remote/worker.c:28-73` (serve) | CLOSED（可测试路径）：fd 类型分开 poll、锁外等待/休眠、MSG_NOSIGNAL；TCP/无 HCA 回退与重复测试。双监听真实 RDMA BLOCKED。 |
| 7 | CONFIRMED | `src/worker/uring_io.c:120-216` (transfer)；`src/worker/uring_io.c:99-118` (quiesce) | CLOSED（缓冲安全）：SQE/CQE/submit/wait/持续 EINTR 有退出；Ring 自有缓冲、取消与未知完成隔离，Direct 文件失败后禁止偏移复用。内核永久卡死时保留隔离资源，回收不宣称无条件成功。 |
| 8 | CONFIRMED | `src/worker/direct_io.c:42-92` (mi_direct_create)；`src/worker/direct_io.c:192-249` (mi_direct_record) | CLOSED：depth/对齐/大小检查；NONE 头校验与 READ_BACK 载荷校验、损坏测试。 |
| 9 | CONFIRMED | `src/worker/tiered_storage.c:269-329` (lease_complete)；`src/worker/paged_kv.c:313-323` (page_complete) | FIXED / FAIL-CLOSED RETENTION：无法确认完成时Storage持有物理资源，清理writer/count与调用者lease/page ref；后端/池延后销毁，禁止复用。需进程退出回收隔离资源，非自动设备恢复。 |
| 10 | CONFIRMED | `src/worker/tiered_storage.c:417-506` (mi_storage_migrate)；`src/worker/tiered_storage.c:702-718` (clear_cleanup) | CLOSED：目标完成前保留原 slot/replica，原子提交、失败回滚；双 cleanup 句柄不能被下一迁移覆盖，retry 后继续。4 线程压力。 |
| 11 | REPRODUCED | `src/worker/allocator.c:271-283` (bin_find)；`src/worker/allocator.c:519-592` (allocate) | CLOSED（TLSF 量化语义）：向上取整、两级 bitmap、无空闲链表扫描；原假 ENOMEM 复现、30 万轮随机及整池合并。 |
| 12 | CONFIRMED | `src/worker/device_cuda.cu:148-163` (copy)；`src/worker/tiered_storage.c:649-695` (mi_storage_read_snapshot_event)；`src/worker/storage_backend.c:379-403` (mi_backend_gpu_copy)；`src/worker/graph_allocator.c:243-272` (mi_graph_allocator_destroy) | FIXED / FAIL-CLOSED RETENTION：GPU快照使用后端自有Host缓冲，确认后才复制给调用者；Direct/Remote及迁移在未知完成时保留两端所有者；Graph/后端持池引用。真实GPU故障注入和64并发通过，永久隔离的保留量单列。 |
| 13 | CONFIRMED | `src/worker/spdk_bridge.c:149-156` (poll_shutdown)；`src/worker/spdk_bridge.c:286-297` (mi_spdk_close) | CLOSED（malloc bdev）：owner-thread poller stop、初始化失败与 caller send_msg 错误注入；物理 SPDK NVMe BLOCKED。 |
| 14 | CONFIRMED | `src/integration/vllm_bridge.c:100-162` (kvf_vllm_open) | CLOSED（方法/C bridge）：指标锁、Future 异常、partial block/prior、stride 预算；10 Python 方法测试+真实 C bridge 8 页往返。完整 vLLM 集成 BLOCKED。 |
| 15 | CONFIRMED | `src/protocol/kvs_text.c:16-77` (kvs_text_command) | CLOSED：未认证远程 SAVE/LOAD 全拒绝，内部持久化安全故障覆盖保留。兼容性收紧有记录。 |
| 16 | CONFIRMED | `src/replication/kvs_auth.c:37-67` (kvs_transport_authenticate)；`src/replication/kvs_master.c:279-296` (accept_slaves) | CLOSED（已覆盖威胁）：V2 双向随机 nonce/HMAC、防重放、1 秒握手、64 副本及既有队列/快照预算，未认证不生成快照。复制数据仍为明文。 |
| 17 | CONFIRMED | `src/protocol/kvs_resp.c:163-179` (kvs_resp_buffer_bulk)；`src/network/kvs_connection.c:17-34` (grow) | CLOSED：明确回复容量/所有权，溢出先校验，32 KiB GET 与 64 MiB 上限；小回复原 API 保留。 |
| 18 | CONFIRMED | `src/store/kvs_data.c:175-265` (kvs_store_mset) | CLOSED（内存/Store 锁）：批准备后无分配提交，四引擎穷举 33 个失败点；DEL/UNLINK 无分配路径。多记录 WAL 崩溃事务不在该保证内。 |
| 19 | CONFIRMED | `src/replication/kvs_transport.c:61-97` (kvs_transport_wait) | CLOSED（确定性测试）：绝对 deadline + zero-time rpoll + 有界休眠；RDMA=1 链接替身验证，真实 HCA BLOCKED。 |



### 首轮未闭环问题与生产差距（历史记录，当前处置见后续修复）

- #9/#12：现有同步 API 借用调用者内存，错误返回后未必有可长期保留的所有权对象；当 Stream/Event 都不能确认结束时，既不能回收又不能无限阻塞。需要把失败租约、页引用、staging、backend/pool 销毁都纳入明确的退休接管协议，并以设备故障/恢复测试验证。本轮保留安全 fence 与 active 状态，未以清零引用计数掩盖未完成 GPU 工作，也未进行设备 reset 或全局同步。**不签署该路径“无悬空访问/无泄漏”的验收结论。**
- io_uring 未确认完成时隔离 Ring 及其自有缓冲，Direct 后端安全失败并要求重建。永久内核/设备故障时会保留资源，需进程级处置与更完整的回收策略；测试注入可取消错误均无 Sanitizer 泄漏。
- `src/worker/kv_cache.c` 原有 cache mutex 仍跨部分 promote/迁移 I/O，本轮未无边界重写该模块。Storage/Remote 本轮修改未新增核心元数据锁内 I/O，但不能把该结论扩大为全仓所有元数据锁均无 I/O。
- Legacy 多记录 WAL 不提供批次崩溃原子性；未认证客户端普通数据访问仍依赖部署隔离，V2 HMAC 仅认证复制握手，不提供 TLS 加密、密钥轮换或全局内存背压。
- 没有真实 HCA/SoftRoCE、多节点网络与物理 SPDK NVMe、完整 vLLM 环境的验收。当前单机合成/loopback 性能不代表生产 LLM 负载或跨机 SLA。


### 复核精度补充

- #13 的真实缺陷依据是 send_msg 失败后跳过 `stop(d)` 的 channel/desc/DMA 清理，以及 ENOMEM 无界重试。当前 SDK 的 `event.h` 没有将 `spdk_app_stop` 本身限定为只能在 owner thread 调用；不能单凭“从调用线程调用 app_stop”就认定 API 越界。`spdk_put_io_channel` 的同线程要求则在 SDK `thread.h:723-730` 明确存在。本轮通过 owner poller 完成全部清理，既消除已证实遗漏，也满足更强的线程归属不变量。
- MMPOOL 补测最初发现 `test_audit_atomic` 未调用可选 allocator 初始化（libc 不需要但 MMPOOL 需要），为新增测试夹具缺陷；补齐 init/destroy 后，MMPOOL 的四引擎失败原子性、快照和 allocator 测试全部通过。libc ASan/TSan 相同断言复验通过，未变更生产分配器。
- 完成 io_uring 双重故障注入：wait、sync-cancel、peek 同时失败时进入保留隔离分支，调用者原 buffer 可释放；恢复后再次由原 owner 销毁成功，ASan/LSan/TSan 通过。详细日志 `final-uring-quarantine.log`。永久不恢复时仍保留资源，不虚构自动回收。


## Phase 6：首轮交付（历史记录）

- [完整验证报告](KV-FABRIC-VALIDATION-REPORT.md) 已落盘，包含环境、全量/Sanitizer/故障/压力矩阵、Phase 0与最终单轮全部性能数值、交替复测、修复额外拷贝的证据及未通过条件。
- [原始证据与复现入口](benchmarks/audit-2026-11-26/README.md) 已保存到仓库，约1.4 MB；测试源码保留在tests，命令/CSV/失败历史/源码范围/二进制与文件哈希均可审阅。portable性能入口在新/tmp目录实跑成功。
- 最终主矩阵：Release18/18、CUDA/uring23/23、ASan/UBSan/LSan19/19、TSan19/19、SPDK20/20、纯TCP19/19、Storage-only10/10、Python15/15、Legacy7个单元入口。新增测试还完成了可恢复内核三重错误隔离与MMPOOL初始化兼容复验。
- 复核标签：REPRODUCED5、CONFIRMED14；PATH_CHANGED为附加标签；17项有本轮修复及适用验证，#9/#12保留PARTIAL/OPEN HIGH。原报告中部分已存在加固、SDK API推断不成立的子判断已明确修正，不重复计为全新修复。
- 最终4对恢复对照：GPU→Host -0.31%、Host→SSD -2.39%、SSD→GPU -1.54%、吞吐+1.56%；确定性>5%拷贝回退消除。原始单轮及尾部短样本仍有>5%漂移，不能把总体中位数的稳定性当严格全指标零回退保证。
- 生产阻断仍包括连续GPU错误退休协议、真实RDMA与物理SPDK/完整vLLM未验证、目标负载稳定性能SLA等。审计与文档交付完成，不等于全部生产验收条件通过。
- HEAD/分支与启动时相同，暂存区补丁逐字节未变。没有commit/push、没有覆盖6个外部更新文件；完整原始工作区归档仍在/tmp。


## 后续修复：用户授权提交与上传 GitHub

- 用户追加“修复并上传GitHub”，现已授权提交与发布；仍保留原工作区与暂存区，不在该工作区切分支。
- #9/#12 新增安全失败语义：Stream/Event均不能确认完成时，由Storage保留物理资源，清空调用方lease及writer/count，让页引用正常收尾。隔离实例拒绝读写、迁移与复用，销毁仍保留Storage、绑定后端及池，恢复要求进程重启。
- GPU后端使用64个固定、4KiB对齐的自有pinned传输槽，与最大64个Worker lanes一致（每后端额外64×align_up(block_bytes,4096)，另有至多4095字节基址对齐开销）；快照仅在确认完成后复制给调用者，错误返回不会遗留向借用目标缓冲的DMA。Direct与Remote导入失败同样保留对应buffer/stream所有者。
- 后端与Graph持有内存池引用；Graph捕获/执行记录未forget时destroy不释放workspace。池事件等待在元数据锁外，trim/destroy不再使用全设备同步。
- 完整复验结果与最新性能见验证报告。新增64线程同时快照、lease/snapshot/read_to_host/双向Direct/GPU到CPU迁移的未知完成故障、父资源销毁后地址存活，以及Graph持有池的回归。异常实例需要进程退出回收，未实现设备在线恢复。
- GitHub已确认目标为sahal-colder/kv-fabric，默认dev与本地HEAD一致；将发布独立codex分支，包含必要的原有项目源码依赖，排除简历、构建物和无关素材。

- 后续性能回修：GPU迁移已持有源/目标所有权，直接搬运到受保护目标；不对该路径增加Host缓冲拷贝。未知完成时两个后端、staging及目标槽一起保留。公开借用Host地址的读取继续使用自有缓冲。CPU热字段和mutex布局保留，隔离状态放在冷字段。

- 最终完整矩阵与有意保留的GPU故障资源量已在验证报告单列。普通memcheck零错误；永久隔离用例不能标为零泄漏PASS。固定P/E核各12对CPU/Page/Directory复核均在3%以内；未固定核Directory+14.16%样本仍保留。

- 后续修复后累计本轮变更85个文件（生产/接口/构建56、测试18、配置/文档11），完整本轮delta和发布快照依赖清单分别保留；原暂存区未变。


## GitHub发布范围与自动审批记录

用户已授权修复并上传。自动审批先拒绝原始审计证据，再拒绝含历史个人备份位置的部署文档。发布范围已收窄为代码、测试、通用部署说明与不含主机身份/绝对路径/备份哈希的公开摘要；完整本地两份报告和原始证据保留，公开同名报告为脱敏摘要。

审批拒绝前有部分证据作为未引用Git tree/blob写入远程；没有为这些树创建发布分支。最终公开树从仅源码树重建，不引用原始证据对象；现有接口不能直接删除GitHub未引用Git对象。最终公开树为eae120064f9d20aff04cba5c6ace5d6c452190c6，提交11d744f92aac81b34d5401578ebc1dde32a1fe09；117个变更文件，未新增简历、模型、构建目录或原始审计证据。

- GitHub发布已完成：[草稿PR #1](https://github.com/sahal-colder/kv-fabric/pull/1)，分支`codex/audit-remediation-20261126`，提交`11d744f92aac81b34d5401578ebc1dde32a1fe09`。公开tree干净导出Release18/18、Legacy单元、Python15/15通过；dev与原工作区暂存区保持原状。


## 完整项目与简历交付进行中

- 当前修复版本完整vLLM CPU/NVMe 8类回归通过，3384个GPU层块；SoftRoCE CPU/CUDA各30轮严格传输通过，临时网络与模块已清理。物理NVMe 72MiB 15样本复验P50 58.465846ms、有效1.202625GiB/s。
- 当前4K/8K/16K四模式矩阵前三种完整模式已保留；最后NVMe模式的启动进程收到终止信号（143），未将缺失16K cell判PASS，原始目录归档后按同参数恢复该模式。
- 已生成完整/公开两个单页A4、正文11pt PDF/DOCX，并经标准渲染逐页检查。完整姓名/联系方式只本地保留；公开版本的XML/超链接/元数据已核验无私人联系方式。
- 已补齐参数化验证归档入口、严格RDMA独立测试、白名单数据导出及GitHub CPU/Sanitizer CI；原工作区HEAD与暂存区未动。完整源代码包和原始证据包分开交付，避免把模型、构建目录或凭据当作源码上传。

- 修后完整矩阵12格/180请求完成，135条非参考请求严格Token一致；CPU/NVMe外部命中均50%，加载与写入错误0。中断模式完整重跑，失败历史保留。


## 完整交付与复验完成

新增矩阵12格/180请求、135条非参考Token严格一致；CPU/NVMe实际外部命中50%，load/write error=0。严格SoftRoCE与完整vLLM、物理NVMe均已完成当前代码复验。公开提交ddb5883fab85a1752635c2394d05c060f9a0cbf7，tree349b4284f90871af3b0907f19618f9a00ae8a177。

本地dist下提供双项目源码与私人简历ZIP，以及单独的原始证据tar.zst；源码包598个文件SHA256核验、ZIP自检和证据zstd完整性检查通过。原模型、构建目录和私有虚拟环境不重复打包；不包含凭据。GitHub只发布经审阅脱敏的简历和测量数据。原工作区HEAD/暂存区保持原状。

- 完整交付已合并到GitHub默认dev：90ea96d611151a4992abc8fbe3c2b129e4dace3d；PR#1已合并，head ddb5883fab85a1752635c2394d05c060f9a0cbf7的Release/Debug Sanitizer远程CI均成功。公开tree349b4284f90871af3b0907f19618f9a00ae8a177与本地独立验证一致。
