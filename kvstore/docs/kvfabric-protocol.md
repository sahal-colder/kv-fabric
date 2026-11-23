# KV-Fabric Wire V3

`kvfabricd` accepts metadata RPCs on `127.0.0.1:9000` by default.
The Worker advertises a separate endpoint for data. All nodes use the same
wire version. A mixed-version peer is rejected during header validation.

## Canonical namespace and page identity

The canonical byte stream starts with ASCII `KVFabric namespace v1`.
It serializes five 32-byte digests in order for model revision, tokenizer revision,
KV layout, adapter revision and tenant salt. Then it encodes tokens per page,
layer count and layer bytes as three u32 values, followed by dtype, TP rank,
TP size, PP rank and PP size as five u16 values. All integers are big-endian.

The namespace key is SHA-256 of the canonical stream. A page digest is SHA-256
of the stream, the previous page digest, then exactly one page of u32 big-endian
token IDs. The first page uses a zero previous digest. Tensor payload layout and
byte order are part of the layout digest.

The manifest records the namespace key, page digest, parent digest, Worker name
and boot UUID, shape and every layer in order. Each layer stores Block ID,
Version, Generation, length and CRC32C. The Generation equals the high word
of the worker-local Block ID. The low word is the slot index plus one.

## Frame header

| Offset | Bytes | Value |
| ---: | ---: | --- |
| 0 | 4 | ASCII `KVF1` |
| 4 | 2 | Version 3 |
| 6 | 2 | Header length 36 |
| 8 | 2 | Opcode |
| 10 | 2 | Flags, bit 0 indicates response |
| 12 | 4 | Status |
| 16 | 8 | Nonzero Request ID |
| 24 | 4 | Payload length |
| 28 | 4 | Payload CRC32C |
| 32 | 4 | CRC32C of the first 32 header bytes |

The codec checks the header, size and receive capacity before reading the payload.
Metadata frames are limited to 8 KiB. Successful Fetch responses can carry up to
4 MiB per layer. Empty payloads have CRC32C zero. Frames use monotonic absolute
deadlines. The I/O loops handle both short transfers and interrupted syscalls.

Control RPCs and the legacy `FETCH` and `BLOCK_META` operations consume one
connection each. `FETCH_STREAM` runs ordered request/reply exchanges on one
connection. Every request carries its own fetch capability and Request ID.
The responder closes idle streams and bounds simultaneous connections by lane count.
Listeners bind to loopback by default. Explicit LAN binds belong on a trusted network.

## Directory replicas and leases

- `REGISTER` binds the endpoint to a name and Boot UUID. A matching heartbeat refreshes
  TTL and reports active transfers and capacity. Registration can replace the
  expected previous boot. A replaced boot loses its replicas and leases.
- `PUBLISH` commits one complete replica under the directory lock. Identical
  manifests from the same boot are idempotent. Different boots can publish
  the same page when parent and layer checksums agree.
- `LOOKUP_PREFIX` finds the longest contiguous visible chain. `ACQUIRE_PREFIX`
  grants the first requested page, prefers the source with the longest chain
  of following pages, then uses the lower heartbeat-reported transfer load.
- Lease validation checks the directory epoch, monotonic fence, slot and secret,
  manifest revision, Worker Boot UUID, layer index, ID, Generation, Version,
  length and checksum. Epoch is fresh on start. Live workers replay their
  publications after re-registering.
- At capacity, the directory protects logical parents and leased replicas.
  It reclaims the least recently used remaining leaf. `RETIRE` checks active
  leases and prefix dependencies before an individual Worker evicts its local
  copy. Administrative `INVALIDATE` explicitly revokes an export and any tickets
  bound to it.

All of these operations finish their metadata critical section before sending a
response. `KVF_FETCH`, `KVF_FETCH_STREAM` and `KVF_BLOCK_META` are data-port
operations. The control plane returns a protocol error for them.

Build and run the golden wire, SHA-256, CRC32C, directory concurrency, eviction
and fencing tests with `make KVFABRIC=1 test_kvfabric`.
