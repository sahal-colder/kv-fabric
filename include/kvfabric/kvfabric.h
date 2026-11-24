/*
 * KV-Fabric control/data-plane values. Structs own inline values, never process
 * addresses, and are serialized by the wire codec. This header introduces no locks.
 */
#ifndef KVFABRIC_H
#define KVFABRIC_H

#include <stddef.h>
#include <stdint.h>

#define KVF_MAX_LAYERS 128u
#define KVF_MAX_LAYER_BYTES (4u * 1024u * 1024u)
#define KVF_MAX_TOKENS_PER_PAGE 4096u
#define KVF_MAX_LOOKUP 64u
#define KVF_MAX_BATCH_PAGES 8u
#define KVF_MAX_BATCH_BYTES (8u * 1024u * 1024u)
#define KVF_WORKER_NAME 48u
#define KVF_ENDPOINT_TEXT 22u

typedef enum {
    KVF_OK = 0,
    KVF_INVALID,
    KVF_NOT_FOUND,
    KVF_STALE,
    KVF_CONFLICT,
    KVF_LIMIT,
    KVF_TIMEOUT,
    KVF_IO,
    KVF_CRC,
    KVF_PROTOCOL,
    KVF_INTERNAL,
    KVF_UNAVAILABLE,
    KVF_CANCELLED
} kvf_status;

typedef struct { uint8_t bytes[16]; } kvf_uuid;
typedef struct { uint8_t bytes[32]; } kvf_hash;

typedef struct {
    uint8_t ipv4[4];      /* Network-order address octets. */
    uint16_t port;       /* Host-order TCP port. Zero is allowed only when binding. */
} kvf_endpoint;

typedef enum {
    KVF_F32 = 1,
    KVF_F16,
    KVF_BF16,
    KVF_FP8
} kvf_dtype;

typedef struct {
    kvf_hash model;             /* Model revision digest. */
    kvf_hash tokenizer;         /* Tokenizer revision digest. */
    kvf_hash layout;            /* Layout and tensor byte-order revision digest. */
    kvf_hash lora;              /* Adapter revision, or zero for no adapter. */
    kvf_hash tenant;            /* Tenant salt digest, used for identity isolation. */
    uint32_t tokens_per_page;   /* All published pages are full. */
    uint32_t layers;            /* Number of independently leased layer blocks. */
    uint32_t bytes_per_layer;   /* Exact physical payload length of each layer. */
    uint16_t dtype;             /* kvf_dtype describing the tensor elements. */
    uint16_t tp_rank;           /* Tensor-parallel rank, strictly less than tp_size. */
    uint16_t tp_size;           /* Nonzero tensor-parallel group size. */
    uint16_t pp_rank;           /* Pipeline-parallel rank, strictly less than pp_size. */
    uint16_t pp_size;           /* Nonzero pipeline-parallel group size. */
} kvf_namespace;

typedef kvf_namespace kvf_identity;

typedef struct {
    kvf_hash space;             /* Digest of the canonical namespace. */
    kvf_hash page;              /* Digest of the chain through this page. */
} kvf_page_key;

typedef struct {
    char name[KVF_WORKER_NAME]; /* Stable, NUL-terminated registry name. */
    kvf_uuid boot;              /* Fresh process identifier, even if name/port repeat. */
} kvf_owner;

typedef struct {
    kvf_owner owner;            /* Registry identity of this heartbeat. */
    kvf_endpoint endpoint;     /* Reachable worker data endpoint for this boot. */
    uint32_t capacity_pages;   /* Maximum page replicas this worker can publish. */
    uint32_t published_pages;  /* Count maintained by the directory for capacity checks. */
    uint32_t active_transfers; /* Snapshot of live serving work used for replica selection. */
    uint64_t local_hits;        /* Resolves with local prefix hits. */
    uint64_t remote_hits;       /* Resolves with verified remote pages. */
    uint64_t reused_tokens;     /* Tokens delivered from either cache path. */
    uint64_t failed_fetches;    /* Remote attempts ending in a transport/lease/data error. */
} kvf_worker_info;

typedef struct {
    uint64_t id;                /* Worker-local logical block ID, never an address. */
    uint64_t version;           /* Nonzero committed write version. */
    uint32_t generation;       /* High word of id, changes with slot reuse. */
    uint32_t size;             /* Must equal the namespace layer size. */
    uint32_t checksum;         /* CRC32C of exactly size immutable bytes. */
} kvf_layer;

typedef struct {
    kvf_page_key key;           /* Same across compatible replicas of this logical page. */
    kvf_hash parent;            /* Previous page digest in the same namespace, zero for root. */
    kvf_owner owner;            /* Boot owning all referenced physical layers. */
    kvf_identity identity;     /* Describes the namespace and complete layer shape. */
    uint32_t token_count;      /* Equals identity.tokens_per_page. */
    uint32_t layer_count;      /* Equals identity.layers. */
    kvf_layer layer[KVF_MAX_LAYERS]; /* Committed, distinct physical IDs in layer order. */
} kvf_manifest;

typedef struct {
    kvf_uuid directory;       /* Directory epoch, refreshed on every daemon start. */
    kvf_uuid secret;          /* Random capability bound to this lease slot and fence. */
    uint64_t fence;            /* Monotonically increases within the issuing epoch. */
    uint32_t slot;             /* Index into a bounded lease table, reusable after expiry. */
} kvf_ticket;

typedef struct {
    kvf_manifest manifest;    /* Replica chosen and pinned by the grant. */
    kvf_endpoint endpoint;    /* Data endpoint for that replica's boot. */
    kvf_ticket ticket;         /* Capability must accompany each layer request. */
    uint32_t ttl_ms;           /* Relative lifetime, measured by the directory clock. */
} kvf_grant;

typedef struct {
    kvf_page_key key;          /* Must match the lease's logical page. */
    kvf_uuid owner_boot;       /* Must match the serving process and directory entry. */
    kvf_ticket ticket;         /* Valid only in the issuing epoch and fence. */
    uint32_t layer_index;      /* Index in the immutable manifest. */
    kvf_layer layer;           /* Complete expected physical identity including generation. */
} kvf_fetch_request;

typedef struct {
    kvf_page_key key;
    kvf_uuid owner_boot;
    kvf_ticket ticket;
    kvf_hash manifest_digest;   /* Canonical manifest, including every layer's generation and version. */
} kvf_batch_page;

const char *kvf_status_string(kvf_status status);
kvf_status kvf_uuid_new(kvf_uuid *uuid);
int kvf_uuid_equal(kvf_uuid a, kvf_uuid b);
int kvf_uuid_zero(kvf_uuid uuid);
int kvf_hash_equal(kvf_hash a, kvf_hash b);
int kvf_hash_zero(kvf_hash hash);
kvf_status kvf_hash_bytes(const void *data, size_t size, kvf_hash *out);
kvf_status kvf_hash_parse(const char *hex, kvf_hash *out);
void kvf_hash_format(kvf_hash hash, char hex[65]);
kvf_status kvf_identity_hash(const kvf_identity *identity, kvf_hash *out);
/* The root page uses a zero previous hash. Token IDs are encoded as big-endian u32. */
kvf_status kvf_page_hash(const kvf_namespace *space, kvf_hash previous, const int32_t *tokens,
                         uint32_t count, kvf_hash *out);
int kvf_page_key_equal(kvf_page_key a, kvf_page_key b);
int kvf_layer_equal(kvf_layer a, kvf_layer b);
int kvf_owner_valid(const kvf_owner *owner);
int kvf_owner_equal(const kvf_owner *a, const kvf_owner *b);
kvf_status kvf_manifest_validate(const kvf_manifest *manifest);
kvf_status kvf_endpoint_parse(const char *text, kvf_endpoint *out);
void kvf_endpoint_format(kvf_endpoint endpoint, char text[KVF_ENDPOINT_TEXT]);
uint32_t kvf_crc32c(const void *data, size_t size);
uint32_t kvf_crc32c_extend(uint32_t crc, const void *data, size_t size);

#endif
