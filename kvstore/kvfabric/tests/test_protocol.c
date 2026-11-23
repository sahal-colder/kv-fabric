/* Wire tests. The owning test closes both socket ends after the writer joins. No metadata locks. */
#define _POSIX_C_SOURCE 200809L
#include "kvfabric/wire.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int fd;
    const uint8_t *bytes;
    size_t size;
} sender;

static void *split_send(void *data) {
    sender *s = data;
    for (size_t i = 0; i < s->size; ++i) {
        assert(send(s->fd, s->bytes + i, 1, 0) == 1);
        struct timespec delay = {.tv_nsec = 100000};
        assert(nanosleep(&delay, NULL) == 0);
    }
    return NULL;
}

static kvf_status receive_bytes(const uint8_t *wire, size_t count) {
    int fd[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    assert(send(fd[0], wire, count, 0) == (ssize_t)count);
    assert(shutdown(fd[0], SHUT_WR) == 0);
    uint8_t payload[8];
    kvf_frame f;
    kvf_status st = kvf_recv_frame(fd[1], &f, payload, sizeof(payload), kvf_now_ms() + 1000);
    assert(close(fd[0]) == 0);
    assert(close(fd[1]) == 0);
    return st;
}

static void update_header_crc(uint8_t *wire) {
    kvf_writer w = {.buffer = wire, .capacity = KVF_HEADER_BYTES, .size = KVF_HEADER_BYTES - 4};
    kvf_put_u32(&w, kvf_crc32c(wire, w.size));
    assert(!w.failed);
}

static void test_frames(void) {
    int fd[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    uint8_t wire[KVF_HEADER_BYTES + 5], payload[8];
    kvf_frame f = {.opcode = KVF_FETCH, .response = 1, .request_id = UINT64_C(0xaabbccdd00000006), .size = 5};
    assert(kvf_send_frame(fd[0], f, "test!", kvf_now_ms() + 1000) == KVF_OK);
    assert(recv(fd[1], wire, sizeof(wire), MSG_WAITALL) == (ssize_t)sizeof(wire));
    const uint8_t expected[] = {
        'K', 'V', 'F', '1', 0, 3, 0, 36, 0, KVF_FETCH, 0, 1, 0, 0, 0, 0,
        0xaa, 0xbb, 0xcc, 0xdd, 0, 0, 0, 6, 0, 0, 0, 5
    };
    assert(memcmp(wire, expected, sizeof(expected)) == 0);
    kvf_reader crc = {.buffer = wire, .size = KVF_HEADER_BYTES, .offset = sizeof(expected)};
    assert(kvf_get_u32(&crc) == kvf_crc32c("test!", 5));
    assert(kvf_get_u32(&crc) == kvf_crc32c(wire, KVF_HEADER_BYTES - 4));

    sender s = {fd[0], wire, sizeof(wire)};
    pthread_t sender_thread;
    assert(pthread_create(&sender_thread, NULL, split_send, &s) == 0);
    assert(kvf_recv_frame(fd[1], &f, payload, sizeof(payload), kvf_now_ms() + 1000) == KVF_OK);
    assert(f.opcode == KVF_FETCH && f.response && f.request_id == UINT64_C(0xaabbccdd00000006));
    assert(f.size == 5 && memcmp(payload, "test!", f.size) == 0);
    assert(pthread_join(sender_thread, NULL) == 0);
    assert(kvf_recv_frame(fd[1], &f, payload, sizeof(payload), kvf_now_ms() + 20) == KVF_TIMEOUT);
    assert(close(fd[0]) == 0);
    assert(close(fd[1]) == 0);

    for (size_t i = 0; i < sizeof(wire); ++i)
        assert(receive_bytes(wire, i) == KVF_IO);
    wire[20] ^= 1;
    assert(receive_bytes(wire, KVF_HEADER_BYTES) == KVF_CRC);
    wire[20] ^= 1;
    const size_t invalid[] = {0, 5, 7, 9, 11, 14, 24};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        uint8_t old = wire[invalid[i]];
        wire[invalid[i]] = 255;
        update_header_crc(wire);
        assert(receive_bytes(wire, sizeof(wire)) == KVF_PROTOCOL);
        wire[invalid[i]] = old;
    }
    uint8_t old[4];
    memcpy(old, wire + 24, 4);
    memset(wire + 24, 255, 4);
    update_header_crc(wire);
    assert(receive_bytes(wire, KVF_HEADER_BYTES) == KVF_PROTOCOL);
    memcpy(wire + 24, old, 4);
    update_header_crc(wire);
    wire[sizeof(wire) - 1] ^= 1;
    assert(receive_bytes(wire, sizeof(wire)) == KVF_CRC);
}

static void *large_send(void *arg) {
    sender *s = arg;
    kvf_frame f = {.opcode = KVF_FETCH, .response = 1, .request_id = 1, .size = (uint32_t)s->size};
    assert(kvf_send_frame(s->fd, f, s->bytes, kvf_now_ms() + 3000) == KVF_OK);
    return NULL;
}

static void test_partial_writes(void) {
    int fd[2], send_buffer = 1024;
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    assert(setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0);
    uint8_t *src = malloc(KVF_MAX_LAYER_BYTES), *dst = malloc(KVF_MAX_LAYER_BYTES);
    assert(src && dst);
    for (size_t i = 0; i < KVF_MAX_LAYER_BYTES; ++i)
        src[i] = (uint8_t)(i * 13 + (i >> 9));
    sender s = {fd[0], src, KVF_MAX_LAYER_BYTES};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, large_send, &s) == 0);
    kvf_frame f;
    assert(kvf_recv_frame(fd[1], &f, dst, KVF_MAX_LAYER_BYTES, kvf_now_ms() + 3000) == KVF_OK);
    assert(f.size == KVF_MAX_LAYER_BYTES && memcmp(src, dst, f.size) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(close(fd[0]) == 0);
    assert(close(fd[1]) == 0);
    free(dst);
    free(src);
}

static void test_codec_bounds(void) {
    uint8_t bytes[4] = {0};
    kvf_writer w = {.buffer = bytes, .capacity = sizeof(bytes), .size = SIZE_MAX};
    kvf_put_u32(&w, 1);
    assert(w.failed && bytes[3] == 0);
    kvf_reader r = {.buffer = bytes, .size = sizeof(bytes), .offset = SIZE_MAX};
    assert(kvf_get_u32(&r) == 0 && r.failed);
}

static void test_identity(void) {
    kvf_identity i = {.tokens_per_page = 2, .layers = 2, .bytes_per_layer = 4096,
                      .dtype = KVF_F32, .tp_size = 1, .pp_size = 1};
    assert(kvf_hash_bytes("model", 5, &i.model) == KVF_OK);
    assert(kvf_hash_bytes("tokenizer", 9, &i.tokenizer) == KVF_OK);
    assert(kvf_hash_bytes("layout", 6, &i.layout) == KVF_OK);
    assert(kvf_hash_bytes("tenant", 6, &i.tenant) == KVF_OK);
    kvf_hash space, page, second, other;
    int32_t tokens[] = {1, 131075};
    assert(kvf_identity_hash(&i, &space) == KVF_OK);
    assert(kvf_page_hash(&i, (kvf_hash){0}, tokens, 2, &page) == KVF_OK);
    assert(kvf_page_hash(&i, page, tokens, 2, &second) == KVF_OK);
    assert(kvf_page_hash(&i, page, tokens, 1, &other) == KVF_INVALID);
    char hex[65];
    kvf_hash_format(space, hex);
    assert(strcmp(hex, "b1120d35a7b4ac43b2803a29ee07eca32fe080ff24ed706beb44bda2b67ed9b1") == 0);
    kvf_hash_format(page, hex);
    assert(strcmp(hex, "ff4e83973ea0f1a61a487e5e1186942550bd83f268334b7b1d6cd2ea9b6590f2") == 0);
    kvf_hash_format(second, hex);
    assert(strcmp(hex, "aa7f4b0b2aca2e0a81517f7a499f1c578475e63831d5549cfa9ad0d3c17f2e45") == 0);
    assert(kvf_hash_parse(hex, &other) == KVF_OK && kvf_hash_equal(second, other));
    assert(!kvf_hash_equal(page, second));
    ++i.tenant.bytes[0];
    assert(kvf_identity_hash(&i, &other) == KVF_OK && !kvf_hash_equal(other, space));
    assert(kvf_crc32c("123456789", 9) == UINT32_C(0xe3069283));
    assert(kvf_crc32c_extend(kvf_crc32c("1234", 4), "56789", 5) == UINT32_C(0xe3069283));
}

int main(void) {
    test_frames();
    test_partial_writes();
    test_codec_bounds();
    test_identity();
    puts("KV-Fabric protocol: PASS");
    return 0;
}
