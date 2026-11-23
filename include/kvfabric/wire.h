/*
 * Wire layer. Codecs borrow caller buffers. TCP functions use absolute monotonic
 * deadlines and enter no Directory/worker locks. A server owns accepted sockets.
 */
#ifndef KVFABRIC_WIRE_H
#define KVFABRIC_WIRE_H

#include "kvfabric.h"

#define KVF_HEADER_BYTES 36u
#define KVF_WIRE_VERSION 3u
#define KVF_METADATA_BYTES 8192u
#define KVF_DEFAULT_TIMEOUT_MS 3000u

typedef enum {
    KVF_REGISTER = 1,
    KVF_HEARTBEAT,
    KVF_UNREGISTER,
    KVF_WORKER_STATUS,
    KVF_PUBLISH,
    KVF_LOOKUP_PREFIX,
    KVF_LOOKUP_PAGE,
    KVF_ACQUIRE,
    KVF_RENEW,
    KVF_RELEASE,
    KVF_INVALIDATE,
    KVF_VALIDATE,
    KVF_STATS,
    KVF_ACQUIRE_PREFIX,
    KVF_RETIRE,
    KVF_FETCH = 32,
    KVF_BLOCK_META,
    KVF_FETCH_STREAM
} kvf_opcode;

typedef struct {
    kvf_opcode opcode;
    int response;
    kvf_status status;
    uint64_t request_id;
    uint32_t size;
} kvf_frame;

typedef struct {
    uint8_t *buffer;
    size_t capacity, size;
    int failed;
} kvf_writer;

typedef struct {
    const uint8_t *buffer;
    size_t size, offset;
    int failed;
} kvf_reader;

void kvf_put_bytes(kvf_writer *w, const void *data, size_t size);
void kvf_put_u16(kvf_writer *w, uint16_t n);
void kvf_put_u32(kvf_writer *w, uint32_t n);
void kvf_put_u64(kvf_writer *w, uint64_t n);
void kvf_get_bytes(kvf_reader *r, void *data, size_t size);
uint16_t kvf_get_u16(kvf_reader *r);
uint32_t kvf_get_u32(kvf_reader *r);
uint64_t kvf_get_u64(kvf_reader *r);
int kvf_reader_done(const kvf_reader *r);
void kvf_put_identity(kvf_writer *w, const kvf_identity *identity);
void kvf_put_owner(kvf_writer *w, const kvf_owner *owner);
void kvf_get_owner(kvf_reader *r, kvf_owner *owner);
void kvf_put_endpoint(kvf_writer *w, kvf_endpoint endpoint);
void kvf_get_endpoint(kvf_reader *r, kvf_endpoint *endpoint);
void kvf_put_worker(kvf_writer *w, const kvf_worker_info *worker);
void kvf_get_worker(kvf_reader *r, kvf_worker_info *worker);
void kvf_put_key(kvf_writer *w, kvf_page_key key);
void kvf_get_key(kvf_reader *r, kvf_page_key *key);
void kvf_put_layer(kvf_writer *w, kvf_layer layer);
void kvf_get_layer(kvf_reader *r, kvf_layer *layer);
void kvf_put_manifest(kvf_writer *w, const kvf_manifest *manifest);
void kvf_get_manifest(kvf_reader *r, kvf_manifest *manifest);
void kvf_put_ticket(kvf_writer *w, kvf_ticket ticket);
void kvf_get_ticket(kvf_reader *r, kvf_ticket *ticket);
void kvf_put_fetch(kvf_writer *w, const kvf_fetch_request *request);
void kvf_get_fetch(kvf_reader *r, kvf_fetch_request *request);

uint64_t kvf_now_ms(void);
kvf_status kvf_connect(kvf_endpoint endpoint, uint64_t deadline, int *fd);
kvf_status kvf_send_frame(int fd, kvf_frame frame, const void *payload, uint64_t deadline);
kvf_status kvf_recv_frame(int fd, kvf_frame *frame, void *payload, size_t capacity,
                          uint64_t deadline);
kvf_status kvf_rpc(kvf_endpoint endpoint, kvf_opcode opcode, const kvf_writer *request,
                    void *reply, size_t reply_capacity, size_t *reply_size, uint32_t timeout_ms);
/* A borrowed fd must have a single caller until the response is read. */
kvf_status kvf_rpc_on(int fd, kvf_opcode opcode, const kvf_writer *request,
                       void *reply, size_t reply_capacity, size_t *reply_size, uint64_t deadline);

/* Fixed lanes and a bounded kernel accept queue. Handler decides connection lifetime. */
typedef struct kvf_server kvf_server;
typedef void (*kvf_connection_fn)(void *context, unsigned lane, int fd, uint64_t deadline);

kvf_status kvf_server_start(kvf_endpoint listen, unsigned lanes, uint32_t timeout_ms,
                             kvf_connection_fn handle, void *context, kvf_server **out);
kvf_endpoint kvf_server_endpoint(const kvf_server *server);
void kvf_server_stop(kvf_server *server);

#endif
