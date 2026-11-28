/* Real multi-process E2E. The parent owns child PIDs, pipes and fixtures. No shared locks. */
#define _POSIX_C_SOURCE 200809L
#include "kvfabric/client.h"
#include "kvfabric/wire.h"
#include <errno.h>
#include "../../tools/fixture.h"
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct { pid_t pid; int output; } child;

static child spawn(char *const argv[]) {
    int fds[2];
    if (pipe(fds) < 0)
        return (child){0, -1};
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0)
            _exit(126);
        close(fds[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        return (child){0, -1};
    }
    return (child){pid, fds[0]};
}

static int stop(child *p) {
    if (!p->pid)
        return 1;
    close(p->output);
    kill(p->pid, SIGTERM);
    for (unsigned try = 0; try < 200; ++try) {
        int status;
        if (waitpid(p->pid, &status, WNOHANG) == p->pid) {
            p->pid = 0;
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        nanosleep(&(struct timespec){.tv_nsec = 10000000}, NULL);
    }
    kill(p->pid, SIGKILL);
    while (waitpid(p->pid, NULL, 0) < 0 && errno == EINTR) {}
    p->pid = 0;
    return 0;
}

static int read_line(child p, char *line, size_t capacity) {
    if (!p.pid)
        return 0;
    uint64_t limit = kvf_now_ms() + 20000;
    for (size_t i = 0; i + 1 < capacity;) {
        uint64_t now = kvf_now_ms();
        if (now >= limit)
            return 0;
        struct pollfd fd = {.fd = p.output, .events = POLLIN};
        int ready = poll(&fd, 1, (int)(limit - now));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || read(p.output, &line[i], 1) != 1)
            return 0;
        if (line[i++] == '\n') {
            line[i] = '\0';
            return 1;
        }
    }
    return 0;
}

static int reap(child *p) {
    int status;
    pid_t result;
    do {
        result = waitpid(p->pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    close(p->output);
    p->pid = 0;
    return result > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int pull(char *const args[], int available) {
    child importer = spawn(args);
    char line[256];
    if (!read_line(importer, line, sizeof(line))) {
        stop(&importer);
        return 0;
    }
    int rc = reap(&importer);
    const char *miss = "PULL pages=0 local_tokens=0 remote_tokens=0 cached_tokens=0 status=";
    if (available ?
        rc == 0 && !strcmp(line, "PULL pages=2 local_tokens=0 remote_tokens=8 cached_tokens=8 status=ok\n") :
        rc == 3 && !strncmp(line, miss, strlen(miss)))
        return 1;
    fprintf(stderr, "import exit=%d output=%s", rc, line);
    return 0;
}

static kvf_fetch_request request_for(kvf_grant g) {
    return (kvf_fetch_request){
        .key = g.manifest.key, .owner_boot = g.manifest.owner.boot, .ticket = g.ticket,
        .layer = g.manifest.layer[0]
    };
}

static int failover(child *publisher, child *daemon, char *daemon_path, char *control,
                    char *const publish_args[], char *const pull_args[], kvf_page_key key) {
    kvf_client c = {.timeout_ms = 300};
    kvf_grant old, current;
    if (kvf_endpoint_parse(control, &c.endpoint) != KVF_OK ||
        kvf_client_acquire(c, key, &old) != KVF_OK)
        return 0;
    kvf_fetch_request stale = request_for(old);
    if (kill(publisher->pid, SIGKILL) != 0 || reap(publisher) != -1 || !pull(pull_args, 0))
        return 0;
    *publisher = spawn(publish_args);
    char line[256], bytes[4096];
    if (!read_line(*publisher, line, sizeof(line)) || strncmp(line, "READY ", 6) ||
        kvf_client_acquire(c, key, &current) != KVF_OK ||
        kvf_uuid_equal(old.manifest.owner.boot, current.manifest.owner.boot) ||
        kvf_client_fetch(current.endpoint, 500, &stale, bytes, sizeof(bytes)) != KVF_STALE ||
        !pull(pull_args, 1))
        return 0;
    puts("SIGKILL and Worker restart: old boot denied, fresh import verified");

    stale = request_for(current);
    if (!stop(daemon))
        return 0;
    char *args[] = {daemon_path, "--listen", control, NULL};
    *daemon = spawn(args);
    if (!read_line(*daemon, line, sizeof(line)) || strncmp(line, "READY ", 6))
        return 0;
    kvf_grant fresh;
    uint64_t limit = kvf_now_ms() + 6000;
    while (kvf_client_acquire(c, key, &fresh) != KVF_OK) {
        if (kvf_now_ms() >= limit)
            return 0;
        nanosleep(&(struct timespec){.tv_nsec = 40000000}, NULL);
    }
    if (kvf_uuid_equal(stale.ticket.directory, fresh.ticket.directory) ||
        !kvf_uuid_equal(fresh.manifest.owner.boot, current.manifest.owner.boot) ||
        kvf_client_fetch(fresh.endpoint, 500, &stale, bytes, sizeof(bytes)) != KVF_STALE ||
        kvf_client_release(c, fresh.ticket) != KVF_OK || !pull(pull_args, 1))
        return 0;
    puts("Directory restart: new epoch, Worker republish, old lease denied");
    return 1;
}

static int exercise(const char *dir, char *worker, char *device, char *control, size_t layer_bytes,
                    child *daemon, char *daemon_path, int restart, int rdma_unavailable) {
    child publisher = {0};
    int ok = 0;
    char ns[256], tokens[256], payload[256], result[256];
    snprintf(ns, sizeof(ns), "%s/namespace", dir);
    snprintf(tokens, sizeof(tokens), "%s/tokens", dir);
    snprintf(payload, sizeof(payload), "%s/tensors", dir);
    snprintf(result, sizeof(result), "%s/target", dir);
    kvf_identity identity;
    if (!kvf_fixture_identity(layer_bytes, &identity) || !kvf_fixture_write(dir, layer_bytes))
        goto done;

    char *publish_args[] = {worker, "serve", "source", control, "127.0.0.1:0",
                            device, ns, tokens, payload, rdma_unavailable ? "--kv-transport" : NULL,
                            "rdma", NULL};
    publisher = spawn(publish_args);
    char line[256];
    if (!read_line(publisher, line, sizeof(line)) || strncmp(line, "READY ", 6))
        goto done;

    char *pull_args[] = {worker, "pull", "target", control, "127.0.0.1:0",
                         device, ns, tokens, result, rdma_unavailable ? "--kv-transport" : NULL,
                         "rdma", "--no-kv-rdma-fallback", NULL};
    if (rdma_unavailable) {
        child strict = spawn(pull_args);
        if (!strict.pid || reap(&strict) != 1)
            goto done;
        pull_args[11] = "--kv-rdma-fallback";
    }
    if (!pull(pull_args, 1))
        goto done;
    if (restart) {
        kvf_page_key key;
        if (kvf_identity_hash(&identity, &key.space) != KVF_OK ||
            kvf_page_hash(&identity, (kvf_hash){0}, kvf_fixture_tokens, TOKENS_PER_PAGE, &key.page) != KVF_OK ||
            !failover(&publisher, daemon, daemon_path, control, publish_args, pull_args, key))
            goto done;
    }
    kvf_hash sha;
    if (!kvf_fixture_check(layer_bytes, result, &sha))
        goto done;
    char hex[65];
    kvf_hash_format(sha, hex);
    printf("device=%s layer_bytes=%zu pages=%d layers=%d sha256=%s fp32_attention=byte-identical\n",
           device, layer_bytes, PAGES, LAYERS, hex);
    ok = 1;

done:
    ok = stop(&publisher) && ok;
    unlink(ns);
    unlink(tokens);
    unlink(payload);
    unlink(result);
    return ok;
}

int main(int argc, char **argv) {
    int restart = argc == 5 && !strcmp(argv[4], "restart");
    int rdma_unavailable = argc == 5 && !strcmp(argv[4], "rdma-unavailable");
    if (argc != 4 && !restart && !rdma_unavailable)
        return 2;
    char dir[] = "/tmp/miniinfer-kvf-e2e.XXXXXX";
    if (!mkdtemp(dir))
        return 1;
    char *daemon_args[] = {argv[1], "--listen", "127.0.0.1:0", NULL};
    child daemon = spawn(daemon_args);
    char line[128], control[KVF_ENDPOINT_TEXT];
    int ok = read_line(daemon, line, sizeof(line)) &&
             sscanf(line, "READY %21s", control) == 1;
    const size_t sizes[] = {4096, 65536, 262144, 1048576, 4194304};
    for (size_t i = 0; ok && i < (restart || rdma_unavailable ? 1 : sizeof(sizes) / sizeof(*sizes)); ++i) {
        ok = exercise(dir, argv[2], argv[3], control, sizes[i], &daemon, argv[1], restart, rdma_unavailable);
        if (!ok)
            fprintf(stderr, "multi-process test failed for %s at %zu bytes\n", argv[3], sizes[i]);
    }
    ok = stop(&daemon) && ok;
    rmdir(dir);
    return ok ? 0 : 1;
}
