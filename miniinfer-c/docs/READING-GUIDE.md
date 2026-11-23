# KV-Fabric 从 0 阅读代码指南

从 4 KiB、两页、每页两层的 CPU 闭环开始。先运行
`ctest --test-dir "$scratch/worker" -R remote_e2e_cpu -V`，看进程入口、
TCP 导入和本地再次命中的顺序，再把设备切到 CUDA。部署命令见
[数据面说明](kvfabric-data-plane.md)。

下文的控制面文件位于 `kvstore/kvfabric/`，数据面文件位于
`miniinfer-c/products/miniinfer-storage/`。

## 1. 数据结构

| 文件 | 建议从这里读 | 需要记住的约束 |
| --- | --- | --- |
| `include/kvfabric/kvfabric.h` | `kvf_namespace`、`kvf_manifest`、`kvf_fetch_request` | Namespace 隔离兼容域。Manifest 拥有所有 Layer 元数据。票据关联 Epoch、Fence、Worker Boot 和层的物理身份。 |
| `src/identity.c` | `kvf_identity_hash`、`kvf_page_hash` | 明确的字段顺序和大端编码参与 SHA-256。页摘要链接上一页和本页 Token。 |
| `include/miniinfer/common.h`、`tiered_storage.h` | `mi_block_meta`、`mi_lease` | Block ID 的高 32 位是 Generation。WRITE 完成后递增 Version。READ 钉住驻留地址。 |
| `include/miniinfer/paged_kv.h`、`src/paged_kv.c` | `mi_kv_sequence_append_page`、`mi_kv_sequence_publish`、`mi_paged_kv_match` | 页引用来自序列、索引、导出或租约。前缀边连接父页和完整 Token 块，父节点用子节点计数防止提前淘汰。 |

## 2. 帧与 RPC

| 文件 | 入口与阅读顺序 |
| --- | --- |
| `include/kvfabric/wire.h`、`src/wire.c` | 从 `kvf_put_u32` 和 `kvf_get_u32` 看边界校验，再读 `kvf_put_manifest` 和 `kvf_get_fetch`。 |
| `src/crc32c.c` | `kvf_crc32c_extend` 可继续已有 CRC。x86 可用 SSE4.2，软件路径给出相同结果。 |
| `src/net.c` | `kvf_connect` → `kvf_rpc_on` → `kvf_send_frame` / `kvf_recv_frame`。对照 `transfer` 看短读写、EINTR、poll 和 Deadline。 |
| `include/kvfabric/client.h`、`src/client.c` | `kvf_client_acquire_prefix` 拿 Grant，`kvf_client_fetch_on` 复用 fd。客户端额外核对请求 ID、层长度和 Manifest CRC。 |

## 3. 控制面

| 文件 | 入口与阅读顺序 |
| --- | --- |
| `tools/kvfabricd.c`、`tools/kvfctl.c` | `main` 装配监听器或构造客户端。先关闭监听器，再释放 Directory。 |
| `src/control.c`、`include/kvfabric/directory.h` | `kvf_control_start` → `control_request` → `dispatch`。Directory 返回值副本后才回包。 |
| `src/directory.c` | `kvf_directory_register` → `kvf_directory_publish` → `select_replica` → `kvf_directory_acquire_prefix` → `kvf_directory_validate`。随后读 `lease_live`、`reclaim_leaf` 和 `kvf_directory_retire`。 |

Directory 只有一个元数据互斥锁。租约绑定副本槽和修订号。即使槽号复用，
旧 Fence 和修订仍无法引用新 Manifest。对容量压力的条件退休先检查租约，
然后检查叶子条件。异常撤销和进程替换会直接阻止旧票据。

## 4. 设备、Segment 与文件层

| 文件 | 入口与阅读顺序 |
| --- | --- |
| `include/miniinfer/device_transfer.h`、`src/device_transfer.c` | `mi_device_init` → `mi_device_host_alloc` → `mi_device_copy_from_host` → `mi_device_event_wait`。默认支持 CPU，开启 `MINIINFER_WITH_CUDA` 后可使用 CUDA Runtime。 |
| `src/segment_pool.h`、`src/segment_pool.c` | `mi_segment_pool_create` 单次分配连续区，`mi_segment_acquire` 用 CAS 取得独占槽。只在 socket、磁盘或 DMA 完成后归还。 |
| `src/allocator.c`、`src/storage_backend.c` | `mi_mempool_alloc_slab` → `mi_memory_backend_create` → `mi_backend_reserve`。Slab 属于分配池，物理槽表属于 Backend。 |
| `src/tiered_storage.c` | `mi_storage_acquire`、`mi_storage_read_snapshot_event`、`mi_storage_migrate`。先钉住记录和槽，放锁再搬运，最后提交新位置。 |
| `src/direct_io.c`、`src/uring_io.c`、`src/file_io.c` | `mi_direct_create` → `mi_direct_record` → `mi_uring_transfer_fixed`。正路记录直接落盘，异常分支按 CRC、短 I/O 和实际设备支持处理。 |
| `src/io_transfer.h`、`src/storage_crc.h` | 定位 I/O 使用借用区间。KV-Fabric 构建的磁盘和网络共用 SDK CRC32C。 |

## 5. 跨进程调用与淘汰

1. 从 `src/remote/cli.c` 的 `main` 进入 `src/remote/worker.c` 的
   `mi_remote_worker_start`。Worker 拥有心跳、导出表、连接池与 Segment，
   Storage 由调用者持有。
2. `src/remote/cache.c` 的 `mi_remote_publish` 先封存完整页，再建立
   导出引用和 Manifest。控制端确认前由心跳重试，并在目录重启后重发。
3. `mi_remote_resolve` 先命中本地 Trie，再按未命中后缀申请远端副本。
   `src/remote/peer.c` 的 `mi_peer_borrow` 根据 Endpoint 和 Boot UUID
   复用健康连接。新 Boot 淘汰旧的空闲连接，旧活动连接归还时关闭。
4. `src/remote/transfer.c` 的 `mi_remote_serve` 校验票据并克隆页引用。
   Storage 在自身锁内校验 Version 和 Generation，持 READ Lease 做
   锁外拷贝。释放本地 READ 后才发送 Segment。
5. `mi_remote_import` 接收并校验层，CUDA 把当前层放到
   DMA 流后即可接收下一层。两槽在复用前各自等待事件。每层 WRITE
   全部结束后再封页。任一层失败时 `import_page` 截断暂存后缀。
6. `mi_remote_append_page` 容量紧张时只挑未钉住的 LRU 叶子。先通过
   Directory 条件退休，再重新检查本地引用和子节点，最后释放数据。
   索引和导出各持一个所有权引用。活跃的请求、传输和模型租约均会阻止自动淘汰。
7. `src/remote/prefetch.c` 的 `mi_remote_prefetch_async` 将 Token 副本
   放入固定深度 8 的队列。单后台线程走同一解析入口。取消在页或层
   的边界观察。最后用 `wait` 领走 Sequence，或 `destroy` 回收结果。

### 锁顺序

```text
Worker updates → exports_lock → Trie → Storage → 物理槽锁
Directory lock 在单独的控制进程内
Peer pool / Prefetch queue 与 Trie / Storage 锁分开
```

`updates` 串行化发布、退休与心跳。Socket、文件 I/O 和 DMA 等待
均在 Directory、Trie 和 Storage 元数据锁外。负责淘汰的调用在放锁
申请退休前先检查本地候选，在 RPC 返回后再检查一次。

### 对照测试

- `kvfabric/tests/test_protocol.c` 核对帧、SHA-256 和 CRC32C 固定向量。
- `kvfabric/tests/test_directory.c` 核对 RPC 并发、副本路由、TTL、条件退休和 Fence。
- `tests/test_remote_e2e.c` 起真实控制、Producer 和 Consumer 进程，核对五档 FP32。
- `tests/test_remote_faults.c` 注入断连、坏 CRC、过期和驱逐，覆盖预取取消及连接池。
- `tests/test_direct_io.c` 覆盖对齐记录、异步迁移、坏 CRC、截断和槽复用。
- `tools/fixture.c` 是演示和多进程测试共用的 FP32 数据与 Attention 参考。
