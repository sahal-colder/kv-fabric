/* Identity layer. Canonical bytes belong to this call. No persistent state or locks. */
#define _POSIX_C_SOURCE 200809L
#include "kvfabric/wire.h"
#include <arpa/inet.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>

const char *kvf_status_string(kvf_status status) {
    switch (status) {
    case KVF_OK: return "ok";
    case KVF_INVALID: return "invalid argument";
    case KVF_NOT_FOUND: return "not found";
    case KVF_STALE: return "stale identity or lease";
    case KVF_CONFLICT: return "conflict";
    case KVF_LIMIT: return "capacity exceeded";
    case KVF_TIMEOUT: return "timeout";
    case KVF_IO: return "I/O error";
    case KVF_CRC: return "checksum mismatch";
    case KVF_PROTOCOL: return "invalid frame or response";
    case KVF_INTERNAL: return "internal error";
    case KVF_UNAVAILABLE: return "local data unavailable";
    case KVF_CANCELLED: return "cancelled";
    }
    return "unknown status";
}

kvf_status kvf_uuid_new(kvf_uuid *uuid) {
    if (!uuid)
        return KVF_INVALID;
    if (RAND_bytes(uuid->bytes, sizeof(uuid->bytes)) != 1)
        return KVF_INTERNAL;
    uuid->bytes[6] = (uuid->bytes[6] & 0x0f) | 0x40;
    uuid->bytes[8] = (uuid->bytes[8] & 0x3f) | 0x80;
    return KVF_OK;
}

int kvf_uuid_equal(kvf_uuid a, kvf_uuid b) {
    return memcmp(a.bytes, b.bytes, sizeof(a.bytes)) == 0;
}

int kvf_uuid_zero(kvf_uuid uuid) {
    return kvf_uuid_equal(uuid, (kvf_uuid){0});
}

int kvf_hash_equal(kvf_hash a, kvf_hash b) {
    return memcmp(a.bytes, b.bytes, sizeof(a.bytes)) == 0;
}

int kvf_hash_zero(kvf_hash hash) {
    return kvf_hash_equal(hash, (kvf_hash){0});
}

kvf_status kvf_hash_bytes(const void *data, size_t size, kvf_hash *out) {
    if (!out || (!data && size))
        return KVF_INVALID;
    unsigned len;
    return EVP_Digest(data, size, out->bytes, &len, EVP_sha256(), NULL) == 1 &&
           len == sizeof(out->bytes) ? KVF_OK : KVF_INTERNAL;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

kvf_status kvf_hash_parse(const char *hex, kvf_hash *out) {
    if (!hex || !out || strlen(hex) != 64)
        return KVF_INVALID;
    for (size_t i = 0; i < 32; ++i) {
        int hi = hex_digit(hex[2 * i]);
        int lo = hex_digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return KVF_INVALID;
        out->bytes[i] = (uint8_t)(hi * 16 + lo);
    }
    return KVF_OK;
}

void kvf_hash_format(kvf_hash hash, char hex[65]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        hex[2 * i] = digits[hash.bytes[i] >> 4];
        hex[2 * i + 1] = digits[hash.bytes[i] & 15];
    }
    hex[64] = '\0';
}

static const char namespace_domain[] = "KVFabric namespace v1";
#define CANONICAL_NAMESPACE_BYTES (sizeof(namespace_domain) - 1 + 5 * 32 + 3 * 4 + 5 * 2)

static kvf_status put_namespace(kvf_writer *w, const kvf_namespace *i) {
    if (!i || !i->tokens_per_page || i->tokens_per_page > KVF_MAX_TOKENS_PER_PAGE ||
        !i->layers || i->layers > KVF_MAX_LAYERS ||
        !i->bytes_per_layer || i->bytes_per_layer > KVF_MAX_LAYER_BYTES ||
        i->dtype < KVF_F32 || i->dtype > KVF_FP8 ||
        i->tp_rank >= i->tp_size || i->pp_rank >= i->pp_size ||
        kvf_hash_zero(i->model) || kvf_hash_zero(i->tokenizer) ||
        kvf_hash_zero(i->layout) || kvf_hash_zero(i->tenant))
        return KVF_INVALID;
    kvf_put_bytes(w, namespace_domain, sizeof(namespace_domain) - 1);
    kvf_put_identity(w, i);
    return w->failed ? KVF_INTERNAL : KVF_OK;
}

kvf_status kvf_identity_hash(const kvf_identity *i, kvf_hash *out) {
    uint8_t bytes[CANONICAL_NAMESPACE_BYTES];
    kvf_writer w = {.buffer = bytes, .capacity = sizeof(bytes)};
    kvf_status st = put_namespace(&w, i);
    return st == KVF_OK ? kvf_hash_bytes(bytes, w.size, out) : st;
}

kvf_status kvf_page_hash(const kvf_namespace *space, kvf_hash previous, const int32_t *tokens,
                         uint32_t count, kvf_hash *out) {
    if (!space || !tokens || count != space->tokens_per_page)
        return KVF_INVALID;
    uint8_t bytes[CANONICAL_NAMESPACE_BYTES + sizeof(previous.bytes) + 4 * KVF_MAX_TOKENS_PER_PAGE];
    kvf_writer w = {.buffer = bytes, .capacity = sizeof(bytes)};
    kvf_status st = put_namespace(&w, space);
    if (st != KVF_OK)
        return st;
    kvf_put_bytes(&w, previous.bytes, sizeof(previous.bytes));
    for (uint32_t i = 0; i < count; ++i)
        kvf_put_u32(&w, (uint32_t)tokens[i]);
    return w.failed ? KVF_INTERNAL : kvf_hash_bytes(bytes, w.size, out);
}

int kvf_page_key_equal(kvf_page_key a, kvf_page_key b) {
    return kvf_hash_equal(a.space, b.space) && kvf_hash_equal(a.page, b.page);
}

int kvf_layer_equal(kvf_layer a, kvf_layer b) {
    return a.id == b.id && a.version == b.version && a.generation == b.generation &&
           a.size == b.size && a.checksum == b.checksum;
}

int kvf_owner_valid(const kvf_owner *owner) {
    size_t n = strnlen(owner->name, sizeof(owner->name));
    if (!n || n == sizeof(owner->name) || kvf_uuid_zero(owner->boot))
        return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)owner->name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.'))
            return 0;
    }
    return 1;
}

int kvf_owner_equal(const kvf_owner *a, const kvf_owner *b) {
    return kvf_uuid_equal(a->boot, b->boot) && strcmp(a->name, b->name) == 0;
}

kvf_status kvf_manifest_validate(const kvf_manifest *m) {
    kvf_hash space;
    if (!m || !kvf_owner_valid(&m->owner) || kvf_hash_zero(m->key.page) ||
        kvf_hash_equal(m->key.page, m->parent) ||
        kvf_identity_hash(&m->identity, &space) != KVF_OK ||
        !kvf_hash_equal(m->key.space, space) || m->token_count != m->identity.tokens_per_page ||
        m->layer_count != m->identity.layers)
        return KVF_INVALID;
    for (uint32_t i = 0; i < m->layer_count; ++i) {
        const kvf_layer *l = &m->layer[i];
        if (!(uint32_t)l->id || !l->version || l->size != m->identity.bytes_per_layer ||
            l->generation != (uint32_t)(l->id >> 32))
            return KVF_INVALID;
        for (uint32_t j = 0; j < i; ++j)
            if (l->id == m->layer[j].id)
                return KVF_INVALID;
    }
    return KVF_OK;
}

kvf_status kvf_endpoint_parse(const char *text, kvf_endpoint *out) {
    if (!text || !out)
        return KVF_INVALID;
    const char *colon = strchr(text, ':');
    char host[INET_ADDRSTRLEN];
    if (!colon || colon == text || (size_t)(colon - text) >= sizeof(host) || !colon[1])
        return KVF_INVALID;
    size_t len = (size_t)(colon - text);
    memcpy(host, text, len);
    host[len] = '\0';
    kvf_endpoint ep;
    if (inet_pton(AF_INET, host, ep.ipv4) != 1)
        return KVF_INVALID;
    unsigned port = 0;
    for (const char *p = colon + 1; *p; ++p) {
        if (*p < '0' || *p > '9')
            return KVF_INVALID;
        port = port * 10u + (unsigned)(*p - '0');
        if (port > UINT16_MAX)
            return KVF_INVALID;
    }
    ep.port = (uint16_t)port;
    *out = ep;
    return KVF_OK;
}

void kvf_endpoint_format(kvf_endpoint ep, char text[KVF_ENDPOINT_TEXT]) {
    snprintf(text, KVF_ENDPOINT_TEXT, "%u.%u.%u.%u:%u", (unsigned)ep.ipv4[0],
             (unsigned)ep.ipv4[1], (unsigned)ep.ipv4[2], (unsigned)ep.ipv4[3], (unsigned)ep.port);
}
