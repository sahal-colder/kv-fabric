/*
 * 控制面启动入口，进程拥有注册表、目录与监听器。先停监听再释放目录。
 */
#define _POSIX_C_SOURCE 200809L
#include "kvfabric/directory.h"
#include "kvfabric/wire.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    kvf_directory_config directory;
    kvf_endpoint listen;
    uint32_t lanes, timeout_ms;
} config;

static char *trim(char *s) {
    while (isspace((unsigned char)*s))
        ++s;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static int setting(config *c, const char *key, const char *value) {
    if (strcmp(key, "listen") == 0)
        return kvf_endpoint_parse(value, &c->listen) == KVF_OK;
    if (*value < '0' || *value > '9')
        return 0;
    char *end;
    errno = 0;
    unsigned long n = strtoul(value, &end, 10);
    if (errno || *end || !n || n > UINT32_MAX)
        return 0;
    struct { const char *name; uint32_t *p; } values[] = {
        {"lanes", &c->lanes}, {"timeout_ms", &c->timeout_ms},
        {"max_pages", &c->directory.max_pages}, {"max_workers", &c->directory.max_workers},
        {"max_leases", &c->directory.max_leases}, {"worker_ttl_ms", &c->directory.worker_ttl_ms},
        {"lease_ttl_ms", &c->directory.lease_ttl_ms}
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(*values); ++i)
        if (strcmp(key, values[i].name) == 0) {
            *values[i].p = (uint32_t)n;
            return 1;
        }
    return 0;
}

static int read_config(const char *path, config *c) {
    FILE *file = fopen(path, "r");
    if (!file) {
        perror(path);
        return 0;
    }
    char buf[256];
    unsigned line = 0;
    int ok = 1;
    while (fgets(buf, sizeof(buf), file)) {
        ++line;
        if (!strchr(buf, '\n') && !feof(file)) {
            ok = 0;
            break;
        }
        buf[strcspn(buf, "#")] = '\0';
        char *key = trim(buf);
        if (!*key)
            continue;
        char *value = strchr(key, '=');
        if (!value) {
            ok = 0;
            break;
        }
        *value++ = '\0';
        if (!setting(c, trim(key), trim(value))) {
            ok = 0;
            break;
        }
    }
    if (ferror(file))
        ok = 0;
    if (fclose(file) != 0)
        ok = 0;
    if (!ok)
        fprintf(stderr, "%s line %u: invalid setting\n", path, line);
    return ok;
}

int main(int argc, char **argv) {
    config c = {.listen = {{127, 0, 0, 1}, 9000}, .lanes = 16, .timeout_ms = KVF_DEFAULT_TIMEOUT_MS};
    kvf_directory_defaults(&c.directory);
    const char *path = NULL, *listen_on = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) {
            puts("usage: kvfabricd [--config FILE] [--listen IPv4:PORT]");
            return 0;
        }
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
            path = argv[++i];
        else if (!strcmp(argv[i], "--listen") && i + 1 < argc)
            listen_on = argv[++i];
        else {
            fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
            return 2;
        }
    }
    if ((path && !read_config(path, &c)) ||
        (listen_on && !setting(&c, "listen", listen_on)))
        return 2;

    sigset_t signals;
    if (sigemptyset(&signals) || sigaddset(&signals, SIGINT) || sigaddset(&signals, SIGTERM) ||
        pthread_sigmask(SIG_BLOCK, &signals, NULL)) {
        fputs("could not prepare signal handling\n", stderr);
        return 1;
    }
    kvf_directory *d = NULL;
    kvf_server *s = NULL;
    kvf_status st = kvf_directory_create(c.directory, &d);
    if (st == KVF_OK)
        st = kvf_control_start(d, c.listen, c.lanes, c.timeout_ms, &s);
    if (st != KVF_OK) {
        fprintf(stderr, "kvfabricd: %s\n", kvf_status_string(st));
        kvf_directory_destroy(d);
        return 1;
    }
    char text[KVF_ENDPOINT_TEXT];
    kvf_endpoint_format(kvf_server_endpoint(s), text);
    printf("READY %s\n", text);
    int signum, error = fflush(stdout) != 0 || sigwait(&signals, &signum) != 0;
    kvf_server_stop(s);
    kvf_directory_destroy(d);
    return error;
}
