/*
 * 控制面诊断客户端，拥有命令参数和结果副本。RPC 调用无元数据锁。
 */
#include "kvfabric/client.h"
#include "kvfabric/wire.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void usage(FILE *file) {
    fputs("usage: kvfctl [--control IPv4:PORT] stats|worker NAME|page NAMESPACE_SHA256 PAGE_SHA256\n", file);
}

int main(int argc, char **argv) {
    kvf_client c = {.endpoint = {{127, 0, 0, 1}, 9000}, .timeout_ms = KVF_DEFAULT_TIMEOUT_MS};
    int first = 1;
    if (argc >= 3 && strcmp(argv[1], "--control") == 0) {
        if (kvf_endpoint_parse(argv[2], &c.endpoint) != KVF_OK || !c.endpoint.port) {
            usage(stderr);
            return 2;
        }
        first += 2;
    }
    if (argc == first + 1 && !strcmp(argv[first], "--help")) {
        usage(stdout);
        return 0;
    }
    kvf_status st;
    if (argc == first + 1 && strcmp(argv[first], "stats") == 0) {
        kvf_directory_stats data;
        st = kvf_client_stats(c, &data);
        if (st == KVF_OK)
            printf("workers=%" PRIu32 " pages=%" PRIu32 " leases=%" PRIu32
                   " publishes=%" PRIu64 " duplicates=%" PRIu64 " lookups=%" PRIu64
                   " matched_pages=%" PRIu64 " invalidations=%" PRIu64 "\n",
                   data.workers, data.pages, data.leases, data.publishes, data.duplicates,
                   data.lookups, data.matched_pages, data.invalidations);
    } else if (argc == first + 2 && strcmp(argv[first], "worker") == 0) {
        kvf_worker_info info;
        int online;
        st = kvf_client_worker_status(c, argv[first + 1], &info, &online);
        if (st == KVF_OK) {
            char address[KVF_ENDPOINT_TEXT];
            kvf_endpoint_format(info.endpoint, address);
            printf("worker=%s boot=", info.owner.name);
            for (size_t i = 0; i < sizeof(info.owner.boot.bytes); ++i)
                printf("%02x", (unsigned)info.owner.boot.bytes[i]);
            printf(" online=%d endpoint=%s pages=%" PRIu32 "/%" PRIu32
                   " local_hits=%" PRIu64 " remote_hits=%" PRIu64 " reused_tokens=%" PRIu64
                   " failed_fetches=%" PRIu64 "\n", online, address, info.published_pages,
                   info.capacity_pages, info.local_hits, info.remote_hits, info.reused_tokens,
                   info.failed_fetches);
        }
    } else if (argc == first + 3 && strcmp(argv[first], "page") == 0) {
        kvf_page_key key;
        if (kvf_hash_parse(argv[first + 1], &key.space) != KVF_OK ||
            kvf_hash_parse(argv[first + 2], &key.page) != KVF_OK)
            return 2;
        kvf_manifest m;
        kvf_endpoint endpoint;
        st = kvf_client_lookup_page(c, key, &m, &endpoint);
        if (st == KVF_OK) {
            char address[KVF_ENDPOINT_TEXT];
            kvf_endpoint_format(endpoint, address);
            printf("state=READY owner=%s endpoint=%s tokens=%" PRIu32 " layers=%" PRIu32 "\n",
                   m.owner.name, address, m.token_count, m.layer_count);
            for (uint32_t i = 0; i < m.layer_count; ++i) {
                kvf_layer l = m.layer[i];
                printf("layer=%" PRIu32 " id=%" PRIu64 " version=%" PRIu64 " generation=%" PRIu32
                       " bytes=%" PRIu32 " crc32c=%08" PRIx32 "\n",
                       i, l.id, l.version, l.generation, l.size, l.checksum);
            }
        }
    } else {
        usage(stderr);
        return 2;
    }
    if (st != KVF_OK)
        fprintf(stderr, "kvfctl: %s\n", kvf_status_string(st));
    return st == KVF_OK ? 0 : 1;
}
