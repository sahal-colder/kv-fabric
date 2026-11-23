# kvstore

`kvstore` is a small C key-value store with interchangeable in-memory indexes and multiple network event-loop implementations.

Startup configuration, RDMA replication, eBPF acceleration, and the staged MiniInfer
metadata adapter are described in [the integration guide](docs/config-sync-miniinfer.md).
Start with `./build/bin/kvstore --config config/kvstore.conf`;
`--check-config` validates without starting the server. See the dedicated KV-Fabric section for the distributed cache interface.

## Layout

- `include/`: public store and network interfaces.
- `src/main.c`: process startup, engine lifecycle, and network backend selection.
- `src/config/`: validated startup configuration and CLI overrides.
- `src/engine/`: ordered storage mutations, persistence, and replication publication.
- `src/protocol/`: newline-delimited text, RESP framing, and response encoding.
- `src/redis/`: Redis command dispatch and command semantics.
- `src/store/`: array, red-black tree, hash table, skiplist, snapshot, and AOF.
- `src/memory/`: allocator adapter and memory-pool implementation.
- `src/network/`: reactor, proactor, and NtyCo-backed servers.
- `src/replication/`: snapshot handoff, ordered updates, TCP, and optional RDMA and eBPF.
- `tests/`: client-based functional test program.
- `clients/`: example clients in several languages.
- `third_party/NtyCo/`: bundled coroutine library.

## Build

```sh
make
```

All generated files are kept under `build/`:

- `build/obj/<allocator>-<features>/`: objects isolated by allocator and feature flags.
- `build/bin/`: the server and functional test programs.

The default allocator is libc. The availability and link dependencies of jemalloc depend on the distribution. Select the allocator at build time.

```sh
make ALLOCATOR=jemalloc
make ALLOCATOR=libc
make ALLOCATOR=mmpool
```

Application code allocates through `kvs_malloc`, `kvs_calloc`, `kvs_realloc`,
and `kvs_free`. The MMPOOL backend recycles small blocks by size class, releases
large blocks individually, and serializes pool access for cross-thread use.
Run its allocator stress test with:

```sh
make ALLOCATOR=mmpool test_allocator
```

RDMA, eBPF, MySQL and NtyCo are optional (`RDMA=1`, `BPF=1`, `MYSQL=1`,
`NTYCO=1`). The default build includes Reactor and Proactor; `PROACTOR=0`
omits the latter. Run `make RDMA=1 BPF=1 bpf all` to build both replication
extensions and the kernel program after installing their development packages.

## KV-Fabric control plane

```text
miniinfer_worker ── register, publish, acquire, validate ──> kvfabricd
producer Worker ───────────── TCP layer data ────────────> consumer Worker
```

`kvfabricd` maintains worker TTL, immutable page replicas, and bounded leases
on a dedicated TCP endpoint. It binds to `127.0.0.1:9000` by default.
Build the C11 SDK with CMake 3.18+, pthreads and OpenSSL Crypto headers.

```sh
make KVFABRIC=1 test_kvfabric install_kvfabric
./build/kvfabric-sdk/bin/kvfabricd --config kvfabric/config/kvfabric.conf
./build/kvfabric-sdk/bin/kvfctl stats
```

For a LAN deployment, pass `--listen NODE_A_IPV4:9000` and have the producer
listen on an explicit data port. The demo defaults to a CUDA producer on Node A
and a CPU consumer on Node B. Use the same Wire V3 SDK revision on both machines.
The control port and the producer data port need to be reachable by Node B.
See the [Wire V3 protocol](docs/kvfabric-protocol.md) and the
[dual-node build and demo commands](../miniinfer-c/docs/kvfabric-data-plane.md).

## Functional tests

Start the server in one terminal:

```sh
./build/bin/kvstore 2000
```

Run all functional tests from another terminal:

```sh
make test_array test_rbtree test_hash test_skiptable test_protocol test_redis test_replication
./build/bin/test_array 127.0.0.1 2000
./build/bin/test_rbtree 127.0.0.1 2000
./build/bin/test_hash 127.0.0.1 2000
./build/bin/test_skiptable 127.0.0.1 2000
./build/bin/test_protocol 127.0.0.1 2000
./build/bin/test_redis 127.0.0.1 2000
./build/bin/test_replication
```

The Python tests launch isolated servers and exercise network framing, disk
faults, and replica reconnect. To run with ASan and UBSan:

```sh
make -j ALLOCATOR=libc SANITIZE=1
make ALLOCATOR=libc SANITIZE=1 test_unit test_allocator
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1
export UBSAN_OPTIONS=halt_on_error=1
python3 tests/test_reliability.py
KVS_TEST_NETWORK=proactor python3 tests/test_reliability.py
python3 tests/test_sync_e2e.py
```

The C test clients use this output format:

```text
SET Teacher King ==> OK = OK
```

The value before `=` is the actual server output; the value after `=` is the expected output.

## Batch commands

Terminate each text command with LF or CRLF. Multiple text commands can be pipelined on one connection.
The server executes them sequentially and concatenates the individual responses
in the same order:

```text
SET Teacher King
GET Teacher
DEL Teacher
```

The response for this request is:

```text
OK
King
OK
```

Batch execution is sequential and is not transactional: if a later command
fails, earlier commands remain applied. Empty lines are ignored.

## Redis compatibility

The server accepts Redis RESP2 requests. Select the Redis storage engine when
starting the server; `array` remains the default when the second argument is
omitted:

```sh
./build/bin/kvstore 2000 array
./build/bin/kvstore 2000 rbtree
./build/bin/kvstore 2000 hash
./build/bin/kvstore 2000 skiplist
```

Standard clients can then connect to the selected port:

```sh
redis-cli -p 2000 PING
redis-cli -p 2000 SET Teacher King NX
redis-cli -p 2000 GET Teacher
redis-benchmark -p 2000 -t ping,set,get,incr,mset -n 10000
```

Applications built with the hiredis library can use the same RESP2 endpoint.

The Redis-compatible command set currently includes `HELLO`, `PING`, `ECHO`,
`SET` (`NX`, `XX`, and `GET` options), `SETNX`, `GET`, `GETSET`, `GETDEL`,
`APPEND`, `DEL`, `UNLINK`, `EXISTS`, `TOUCH`, `RENAME`, `RENAMENX`, `MSET`,
`MSETNX`, `MGET`, `INCR`, `DECR`, `INCRBY`, `DECRBY`, `STRLEN`, `TYPE`,
`DBSIZE`, `FLUSHDB`, `FLUSHALL`, `SAVE`, `LOAD`, `SELECT`, `INFO`, `COMMAND`,
`CLIENT`, `AUTH` and `QUIT`. Multiple RESP requests in one receive buffer are
processed in order, which supports hiredis pipelining and the
`redis-benchmark -P` option
when the aggregate request and response fit within the configured 1 KiB
network buffer.

The Redis view uses the selected Array, red-black-tree, hash-table, or
skip-list engine and database 0. `INFO` reports the active engine as
`kvstore_engine`.
Redis list, set, sorted-set, stream, scripting and transaction
data types are not implemented yet.

## Replication roles

The default role is `standalone`, so existing startup commands remain valid.
Configure a master or slave explicitly with:

```sh
./build/bin/kvstore 2000 hash --role master
./build/bin/kvstore 2001 hash --role slave \
  --master-host 127.0.0.1 --master-port 2000
```

The slave role is read-only for client commands. Every connection first restores
a complete checksummed snapshot, then applies consecutive, versioned incremental
frames. A snapshot barrier prevents writes from falling between these phases.
Bounded per-slave queues keep network I/O out of the request's storage lock.
Heartbeats detect broken links; reconnects retry until shutdown and replace stale
state with a new full snapshot. `INFO` reports `master_link_status:up` only after
snapshot installation. Replication is asynchronous, without quorum or automatic
failover. Both sides must use the new protocol version. TCP uses the client port
unless `replication-port` selects a separate listener; RDMA requires a separate
listener. See the integration guide for resource budgets and eBPF requirements.

## Persistence backends

Choose one runtime backend in the configuration file:

```text
persistence full
# or: persistence aof
# or: persistence none
```

The modes are mutually exclusive. The header macro only supplies the startup
default. Full snapshots and incremental logs are two different persistence
strategies, not two files that are automatically combined during recovery.

`KVS_PERSISTENCE_FULL` writes a complete RESP2 snapshot:

```text
SAVE [filename]
LOAD [filename]
```

The filename is optional; the default is `data/kvstore.snapshot`. A snapshot
contains a `KVS-META` marker, one `SELECT` section for each enabled engine,
standard Redis `FLUSHDB` and `SET` commands, and a `KVS-END` marker with the
payload length and CRC32. `SAVE` writes a unique temporary file in the target
directory, syncs it, renames it over the snapshot, and syncs the directory.
`LOAD` validates the mapped file and builds replacement indexes before changing
the live store. Full snapshot loading remains explicit. Older binary snapshots
are unsupported.

`KVS_PERSISTENCE_INCREMENTAL` automatically recovers the configured AOF before
serving requests. Each successful mutation is appended as a RESP2 record before
it is acknowledged or replicated. Acknowledgement covers the file write into
the OS cache. Use `SAVE` to synchronize the file to storage. The process stops
without acknowledging the mutation if an append fails after changing memory.
Recovery truncates an incomplete final record and rejects malformed or
unreplayable complete records. `LOAD` uses the same recovery path and installs
the result only after successful replay.

With AOF on a replica, full synchronization atomically replaces the replica's
AOF before the new data is installed and acknowledged. A disconnected replica
can serve previously recovered data until it receives the next snapshot.

MySQL is an independent write log, not a persistence backend:

```sh
make MYSQL=1
./build/bin/kvstore --config config/kvstore.conf --mysql-enabled yes
```

When enabled, successful writes are also appended to the configured MySQL
table. The table is created automatically, and prepared statements keep keys
and values out of SQL text. The request thread only copies the event into a
bounded queue; a dedicated worker reuses one prepared statement and commits
events in batches. MySQL network waits therefore do not hold up client
responses. MySQL is not used by `SAVE` or `LOAD`.
