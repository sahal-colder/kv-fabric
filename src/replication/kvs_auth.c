/* Replication V2 authenticates both endpoints before any snapshot or peer slot.
 * Nonces bind HMAC proofs to one handshake. This does not encrypt cache data. */
#define _POSIX_C_SOURCE 200809L
#include "kvs_transport.h"
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static int exchange(kvs_transport_t *io, void *data, size_t size, int write, uint64_t deadline) {
    uint64_t now = now_ms();
    if (now >= deadline)
        return -1;
    unsigned remaining = (unsigned)(deadline - now);
    return write ? kvs_transport_write(io, data, size, remaining) :
                   kvs_transport_read(io, data, size, remaining);
}

static int proof(const char *secret, unsigned char role, const unsigned char nonces[64],
                 unsigned char out[32]) {
    unsigned char message[65];
    message[0] = role;
    memcpy(message + 1, nonces, 64);
    unsigned length = 0;
    return HMAC(EVP_sha256(), secret, (int)strlen(secret), message, sizeof(message), out, &length)
        && length == 32 ? 0 : -1;
}

int kvs_transport_authenticate(kvs_transport_t *io, const char *secret, int server,
                               unsigned timeout_ms) {
    static const char hello[] = "KVS_SYNC_AUTH_V2\r\n";
    if (!io || !secret)
        return -1;
    size_t length = strnlen(secret, 129);
    if (length < 32 || length > 128)
        return -1;
    unsigned char nonces[64], wire[64], expected[32];
    char magic[sizeof(hello) - 1];
    uint64_t deadline = now_ms() + (timeout_ms < 1000 ? timeout_ms : 1000);
    if (server) {
        if (exchange(io, magic, sizeof(magic), 0, deadline) || memcmp(magic, hello, sizeof(magic)) ||
            RAND_bytes(nonces, 32) != 1 || exchange(io, nonces, 32, 1, deadline) ||
            exchange(io, wire, 64, 0, deadline))
            return -1;
        memcpy(nonces + 32, wire, 32);
        if (proof(secret, 'C', nonces, expected) || CRYPTO_memcmp(expected, wire + 32, 32) ||
            proof(secret, 'S', nonces, wire))
            return -1;
        return exchange(io, wire, 32, 1, deadline);
    }
    if (exchange(io, (void *)hello, sizeof(hello) - 1, 1, deadline) ||
        exchange(io, nonces, 32, 0, deadline) || RAND_bytes(nonces + 32, 32) != 1)
        return -1;
    memcpy(wire, nonces + 32, 32);
    if (proof(secret, 'C', nonces, wire + 32) || exchange(io, wire, 64, 1, deadline) ||
        exchange(io, wire, 32, 0, deadline) || proof(secret, 'S', nonces, expected))
        return -1;
    return CRYPTO_memcmp(expected, wire, 32) ? -1 : 0;
}
