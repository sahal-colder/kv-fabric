from pathlib import Path
import json,xml.etree.ElementTree as ET
root=Path('/tmp/kvf-audit');paired=json.loads((root/'performance-paired.json').read_text());single=json.loads((root/'performance-final-single.json').read_text());recovery=json.loads((root/'performance-accepted-recovery.json').read_text())
report='''# KV-Fabric 验证与验收报告

日期：2026-11-26（Australia/Perth）。对象为 `/home/wty/Infra` 的启动本地工作区，HEAD `55e314292bd6a054fa87ab3de6d07fc6bfe2ed66`，分支 `dev`。**这是有限通过的工程质量改进交付，不是生产级安全认证：#9/#12 永久 GPU 故障路径仍为 PARTIAL / OPEN HIGH。**

## 证据与工作区保护

- 启动时 26 个跟踪修改文件；已修改/未跟踪文件共 278 个，约 1.47 GB。补丁、未跟踪清单、逐文件 SHA-256 与完整归档均保留，未 reset/clean/切分支/commit/push，暂存区未变。
- 保护归档 `/tmp/kvf-audit/preexisting-files.tar`；原始补丁 `/tmp/kvf_pre_audit.patch`，暂存区 `/tmp/kvf_pre_audit_index.patch`。冻结原始业务源码还原到 `/tmp/kvf-audit/original-source`；原始 CPU `build-baseline` 未重建；原始 CUDA 重建在 `/tmp/kvf-audit/original-cuda`。
- 初次与中间失败、全部最终日志、原始 CSV、可复现驱动和源码位置已保存到 [验收证据](benchmarks/audit-2026-11-26/README.md)。原始大归档保留在 /tmp，未把 1.47 GB 用户素材复制进仓库。
- 6 个简历相关原有未跟踪文件在任务期间由外部改动，已单独记录并原样保留，不算本轮修改。完整本轮清单见 [audit-changed-files.json](benchmarks/audit-2026-11-26/audit-changed-files.json)。

## 环境

| 组件 | 实测状态 |
|---|---|
| OS / CPU / RAM | Linux 6.8.0-138-generic；i7-13700H，20 逻辑 CPU；约 15 GiB RAM；ext4 NVMe 文件系统 |
| C / CMake | GCC 11.4.0；C11；CMake 3.22.1；无 clang；OpenSSL 3.0.2 |
| CUDA | Toolkit 13.4.92，驱动 615.71.09，RTX 4060 Laptop 8 GiB，架构 sm_89；真实运行可用 |
| io_uring | liburing 2.9；宿主 queue_init 成功；同步取消要求 liburing >=2.3，CMake 已明确限制 |
| RDMA | librdmacm 1.3.39.0、libibverbs 1.14.39.0 可编译；无 HCA/SoftRoCE，真实数据面 BLOCKED |
| SPDK | 实际 SDK SPDK v27.01-pre 4828014 / DPDK 25.11.0；malloc bdev 实测；物理 SPDK NVMe BLOCKED |
| Python | 3.12.15；pytest 9.1.1；torch 2.11.0+cu128；当前环境无 vLLM、nvtx Python 模块 |
| Sanitizer | GCC ASan/UBSan/LSan 可用；TSan 在 `setarch x86_64 -R` 下可用；GPU 使用 NVIDIA Compute Sanitizer |

最初受限执行环境中 socket/bind 返回 EPERM、GPU 不可见、uring queue_init=-1、LSan 不能运行；这些是执行权限限制。在获准使用本机 loopback/驱动后均已实际重跑，不能把沙箱失败算源码基线失败。

## 构建与全量测试

修改前默认 Release 14/14（15.07 s），CUDA/uring 17/17（37.34 s）。Legacy 原有 5 个单元入口在宿主全部通过。最初沙箱 CTest 8/14、6 个网络失败的原始记录仍保留。

最终适用矩阵：

| 构建 | 配置/入口 | 总数 | 通过 | 失败 | 跳过 | 结果 |
|---|---|---:|---:|---:|---:|---|
'''
for name,file,cmd in [
 ('CPU Release','final-release-ctest.xml','build-release，默认 Release'),
 ('CUDA/uring Release','final-cuda-ctest.xml','build-cuda-release，CUDA/Nano CUDA/uring/bridge，sm_89'),
 ('ASan+UBSan+LSan','final-asan-ctest.xml','build-asan，Debug，KVF_SANITIZE=ON，uring=ON'),
 ('TSan','tsan-ctest.xml','build-tsan，Debug，GCC thread sanitizer，进程 ASLR 关闭'),
 ('SPDK','final-spdk-ctest.xml','build-spdk，SPDK=ON，真实 malloc bdev'),
 ('纯 TCP','tcp-ctest.xml','build-tcp，MINIINFER_WITH_RDMA=OFF')]:
 e=ET.parse(root/file).getroot();tests=int(e.attrib.get('tests',len(e.findall('testcase'))));failed=int(e.attrib.get('failures',0))+int(e.attrib.get('errors',0));skipped=int(e.attrib.get('skipped',0));report+=f'| {name} | {cmd} | {tests} | {tests-failed-skipped} | {failed} | {skipped} | '+('PASS' if not failed else 'FAIL')+' |\n'
report+='''| Storage-only | KVFabric/Nano/Integration=OFF | 10 | 10 | 0 | 0 | PASS |
| Connector 方法 + Legacy 安全 | pytest 精确指定两个文件 | 15 | 15 | 0 | 0 | PASS |
| Legacy 单元 | Make 独立 7 个入口 | 7 | 7 | 0 | 0 | PASS |

- Legacy 的 7 个单元入口在 ASan/UBSan/LSan、TSan 均通过；真实服务安全 5/5 在 Release、ASan、TSan 均通过。
- 既有六类客户端、test_sync_e2e、test_regressions 通过；test_reliability 在 Reactor、Proactor、NtyCo 均通过网络回归；Reactor 的 AOF/复制/快照可靠性在 ASan 和 TSan 下也通过。MMPOOL 的 allocator、多键失败原子性和内部快照测试通过。
- TSan 首轮发现新增 Remote 故障夹具修改 worker immutable 配置的竞态；修复夹具后全量 19/19。没有 suppression、减循环、删断言；首轮失败在 tsan-first-run.*。最新 Direct 隔离状态另外跑了 TSan 三项核心测试×3 轮；最终 io_uring 三重失败隔离/恢复用例在 ASan/TSan 各通过。
- NVIDIA memcheck：Direct CUDA（实际 direct-uring-fixed）与四线程 stream/event 传输，均 **0 errors / 0 bytes leaked**。这是已执行用例的结论，不覆盖任意 CUDA 设备丢失。
- CPU Sanitizer、CUDA memcheck 分开验证；未把 CUDA+TSan、CUDA+ASan 组合或未执行的第三方库组合写成 PASS。
- 生产静态库用 nm 核对，不含 `kvf_directory_test_counters`、`mi_test_*`、`__wrap*` 测试入口。故障注入均为链接 wrap 或测试目标编译宏；Release 没有故障注入分支。

## 并发与故障矩阵

| 场景 | 实测方法 | 结果/边界 |
|---|---|---|
| 同前缀 hide/republish/旧代 invalidate/lookup | 4 线程×10000 轮；新代 Block ID 保留、旧 ticket 失效、最终 leases=0 | PASS；Release 整组重复 20 次，最终 CUDA 配置另重复 10 次；TSan 全量及3轮核心复验 |
| Storage 并发读/写/CPU-SSD 迁移 | 4 线程×10000 轮，校验读无撕裂，最终 lease_count=0 | PASS，同组重复20次，最终配置补测10次 |
| TLSF 随机/碎片/合并/对齐 | 固定种子30万轮，逐字节 canary，整池合并；Release 重复20次 | PASS；查找为两级 bitmap，无空闲链表扫描 |
| 内存分配失败 | allocator 元数据 malloc/calloc 序号注入；Legacy 四索引33个准备失败点 | PASS；原内容、键数、未存在键保持 |
| Lease 过期 | 第一批 FINISH 后等待300 ms，TTL200 ms，再获取第二批 | PASS；旧库复现 STALE，修后完整9页正确 |
| Directory 重启 | 既有 remote_restart 真实进程用例 | PASS |
| RDMA connect/send/recv/reset/partial | 链接 rsocket 替身，实际使用 TCP socket，检查 CRC 与数据 | PASS；不等同 RDMA 硬件验收；batch partial 返回IO、未提交 |
| TCP 超时/取消/损坏 | 既有 remote_faults relay/proxy 与完整性断言 | PASS |
| uring SQE/submit/wait/CQE/EINTR | 测试专用缝隙、真实内核 Ring | PASS；持续中断16次上限；已接受submit失败安全收尾 |
| uring wait+cancel+peek 同时失败 | 保留自有DMA资源，原调用者buffer释放；恢复后原owner再次销毁 | PASS；永久不恢复时资源保留而非强制释放 |
| SSD 复制/清理失败/空间不足 | write/release wrap、旧/新slot/replica检查、retry_cleanup；既有target-full | PASS；原副本不提前丢失，失败目标不提交 |
| Direct NONE/READ_BACK、头/载荷损坏 | depth0/1/64/65，未对齐offset，真ext4文件损坏 | PASS；硬I/O错误隔离整个Direct文件直至重建 |
| CUDA 单次wait失败 | Stream Event补充确认，失败写不提交；双失败active保留 | 部分PASS；永久设备故障跨所有者退休仍未闭环，见#9/#12 |
| GPU线程并发 | 4线程，各100次H2D/D2H，独立Stream/Event | PASS + memcheck |
| SPDK初始化/关闭/send_msg失败 | malloc bdev，stop线程断言，调用线程send_msg注入EIO | PASS；物理NVMe与真实设备移除BLOCKED |
| Connector指标/Future/partial block | 4000条并发JSONL，真实方法体fixture，C bridge真实8页往返 | PASS；完整vLLM调度集成BLOCKED |
| Legacy远程文件/复制认证 | 任意路径/.. /symlink拒绝；70次V1失败无副本；错证明/防重放/合法主从 | PASS；远程SAVE/LOAD有意禁用；握手不加密载荷 |

## 性能方法和完整原始对照

Phase 0 参数始终保留：TLSF 8 MiB/64 B对齐/2000×128 alloc-free/7轮；Page 2层×4096 B，100000次create-append-destroy/7轮；Directory stats 1000 RPC/7轮；SSD和Remote 64 KiB、4客户端、4预热、3×16样本；CUDA 128页×36层=72 MiB、种子734；恢复144×512 KiB、3预热/15样本、逐字节校验。CRC、租约、身份与完整性验证未关闭。

下表“Phase 0/最终单轮”直接对应 `/tmp/kvf_bench_before.txt`、`/tmp/kvf_bench_after.txt`；不删除不利样本。数值为各入口既定输出的中位数；latency越低越好，吞吐越高越好。

| 指标 | Phase 0 | 最终单轮 | 变化 |
|---|---:|---:|---:|
'''
for key,a in single['before'].items():
 b=single['after'][key];report+=f'| {key} | {a:.3f} | {b:.3f} | {(b/a-1)*100:+.2f}% |\n'
report+='''
短基准中多项变化超过5%，不能只报有利的一轮。随后以冻结原始CPU二进制和保护快照重建的原始CUDA二进制交替运行8对CPU/Remote、4对CUDA。每次的参数、数据、预热保持一致；不把新的测量冒称“修改源码前采集”。这些是同源码基准的交替复核。

| 交替复核指标 | 原始中位数 | 修改后中位数 | 变化 |
|---|---:|---:|---:|
'''
for key,v in paired.items():
 if key.startswith('recovery_'):continue
 report+=f'| {key} | {v["before"]:.3f} | {v["after"]:.3f} | {v["change_percent"]:+.2f}% |\n'
report+='''
交替复核仍确认中间实现的SSD→GPU延迟+5.96%、恢复吞吐-5.63%。根因为安全隔离加入的Ring自有缓冲多了一次完整内存拷贝。改为Direct I/O直接使用Ring拥有的CUDA pinned fixed buffer，普通uring仍使用自有复制缓冲保护调用者生命周期。保留全部CRC、租约与回滚语义。

最终实现（含硬I/O后禁止文件偏移复用）的4对交替恢复结果：

| 72 MiB恢复指标 | 原始 | 最终 | 变化 |
|---|---:|---:|---:|
'''
for key,v in recovery.items():report+=f'| {key} | {v["before"]:.3f} | {v["after"]:.3f} | {v["change_percent"]:+.2f}% |\n'
report+='''
判断：已定位的约6%额外拷贝回退已消除。交替组总体中位数中，CPU/Remote/GPU核心指标没有稳定超过5%的回退。但不能签署严格“全部尾延迟均零回退”：Directory逐对变化中位数+5.17%、buffered SSD P99逐对变化中位数+6.60%，与“总体中位数的比值”并不相同。before自身Directory为20.8–27.3 us，buffered吞吐17.9–22.3 GiB/s量级，direct P99为1.09–2.15 ms，明显存在调度/缓存/频率与短样本波动。未触及的buffered I/O/CRC代码也显示同等量级漂移，支持环境噪声推断；未采集足以唯一归因的CPU频率/调度trace，故该推断不当作排除性能风险的证明。严格5% SLA仍需在固定电源/频率/亲和性、足量样本的目标机器复验。

TLSF碎片率仅验证随机负载字节完整性及完全合并后8 MiB整块可分配；没有把“合并后外部碎片0”冒充生产负载碎片率。TLSF请求向上SL量化新增内部开销低于该幂区间的1/32（另加unit对齐）；该分配语义明确记录。

## Nano合成推理补充

用仓库固定seed=7合成模型，同一92-token提示、1预热、12轮、Cold/Local/Remote比较；这是Phase 5用冻结原始源码补充的功能/性能对照，并非Phase 0前已测的生产模型。before/after四组各36样本；所有生成token完全相同。Remote每次命中80/92 token（86.96%），实际Prefill12 token、3次数据请求、20480 B，prefetch失败0；CUDA上下文显存中位98 MiB、设备分配增量4 MiB，修改前后相同。

| 设备/模式 | 原始TTFT P50/P95/P99 (ms) | 最终TTFT P50/P95/P99 (ms) |
|---|---:|---:|
'''
nano=json.loads((root/'performance-nano-detailed.json').read_text())
for dev in ['cpu','cuda']:
 for mode in ['cold','local','remote']:
  a=nano[dev]['before'][mode];b=nano[dev]['after'][mode]
  fmt=lambda v:f'{v["ttft_ns"]/1e6:.3f} / {v["ttft_p95_ns"]/1e6:.3f} / {v["ttft_p99_ns"]/1e6:.3f}'
  report+=f'| {dev}/{mode} | {fmt(a)} | {fmt(b)} |\n'
report+='''
12样本的P95/P99都是最大值，不代表稳定尾延迟；CPU Cold/Local大幅漂移也显示未控制CPU执行条件的局限。没有把它解读为本轮优化带来的几十个百分点推理提升。

## 复现命令

在当前工作区运行（需要允许本地socket、GPU和uring；先不要覆盖build-baseline）：

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j8
ctest --test-dir build-release --output-on-failure

cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DKVF_SANITIZE=ON -DMINIINFER_WITH_URING=ON
cmake --build build-asan -j8
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-cuda-release -DCMAKE_BUILD_TYPE=Release -DMINIINFER_WITH_CUDA=ON -DNANO_WITH_CUDA=ON -DMINIINFER_WITH_URING=ON -DMINIINFER_BUILD_VLLM_BRIDGE=ON -DMINIINFER_VLLM_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-cuda-release -j6
ctest --test-dir build-cuda-release --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DMINIINFER_WITH_URING=ON '-DCMAKE_C_FLAGS=-fsanitize=thread -fno-omit-frame-pointer -fno-pie' '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread -no-pie'
cmake --build build-tsan -j8
TSAN_OPTIONS=halt_on_error=1 setarch "$(uname -m)" -R ctest --test-dir build-tsan --output-on-failure --timeout 180

make -j8 BUILD_DIR=build-legacy-release
KVS_TEST_BUILD_DIR=build-legacy-release python3 -m pytest -q tests/kvstore/test_audit_security.py tests/vllm/test_audit.py
KVS_TEST_BUILD_DIR=build-legacy-release python3 tests/kvstore/test_sync_e2e.py
KVS_TEST_BUILD_DIR=build-legacy-release python3 tests/kvstore/test_reliability.py
```

其他完整配置、故障注入目标与性能命令在证据README/repro目录。测试源已注册在CMake/Make中；不需要Release运行时调试开关。

## BLOCKED / 未验收与生产差距

1. #9/#12 永久GPU故障：同步API借用调用者内存，Stream/Event都不确认完成时不能安全回收；目前未建立跨stack lease/page/staging/backend/pool的持久退休接管协议。部分路径仍保留active而上层可能丢弃栈租约；snapshot/Direct连续错误的引用/缓冲寿命未完整闭环。**OPEN HIGH，不能声明无已知异常生命周期风险。**
2. 真实RDMA/SoftRoCE、多节点断网/拥塞/并发连接恢复：无HCA，BLOCKED。链接替身与TCP回退不能代替硬件验证。
3. 物理SPDK NVMe、真实设备移除/热插拔、hugepage/PCI部署：仅malloc bdev，BLOCKED；没有向任何原始块设备写入。
4. 完整vLLM V1模型调度/Graph跨进程集成：当前Python缺vLLM与nvtx，BLOCKED。方法fixture、C bridge和Nano E2E不冒充该结果。
5. 生产多卡/长上下文模型与Strict 5%性能SLA：单机合成与短loopback样本不足；尾部波动仍未排除，未签署无回退保证。
6. 永久内核I/O故障会隔离保留Ring内存，安全性优先；需运维重建进程/后端与后续回收治理。故障可恢复测试无泄漏不等于永久故障无资源保留。
7. 原有kv_cache mutex跨部分同步迁移、Legacy多记录WAL无崩溃事务、全局背压/控制面TLS认证/HA目录与故障域治理仍是生产差距。

## 验收自检

| 条件 | 结论 |
|---|---|
| Critical/High全部关闭 | **未通过**：#9/#12连续CUDA错误生命周期仍OPEN HIGH |
| 本轮已执行测试无越界/重复释放/竞争报告 | PASS（修复夹具后的适用矩阵）；不扩大为所有故障的证明 |
| 永久死锁/自旋 | 未复现；uring错误和EINTR有退出，SPDK close无send_msg自旋；永久设备调用阻塞不作无条件保证 |
| Remote代数/租约/迁移回滚/TLSF | 适用回归与重复压力PASS，边界见逐项账本 |
| RDMA/CUDA所有异常路径 | **未通过全覆盖**：真实RDMA BLOCKED，CUDA连续错误OPEN |
| Legacy远程文件与未认证复制漏洞 | 已封堵并通过真实服务回归；记录了V2与远程命令兼容性收紧 |
| Release/Sanitizer | 适用矩阵PASS，原失败与修复过程保留 |
| 无无法解释的>5%变化 | **有条件**：确定性拷贝回退已消除；短基准/逐对尾延迟波动仍需受控目标机复核 |
| 文档与数据 | 原始CSV、失败历史、源码范围与哈希清单保留，可复现；未编造BLOCKED结果 |

本轮关闭17项可测试缺陷，另2项部分加固并明确留下生产阻断；共有5项在冻结旧库实际复现。此次交付达到“可构建、可运行、可复测、可解释”的改进审计标准，**尚未达到用户列出的完整生产验收标准**。
'''
Path('/home/wty/Infra/docs/KV-FABRIC-VALIDATION-REPORT.md').write_text(report)
print('Validation report written with raw and paired tables')
