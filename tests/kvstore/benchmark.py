"""Compare equivalent KVStore workloads using redis-benchmark on Linux."""

import argparse
import csv
import os
import re
import socket
import statistics
import subprocess
import tempfile
from pathlib import Path

from test_reliability import resp
from test_sync_e2e import eventually, free_port, request

KEY_COUNT = 8192
VALUE = "v" * 64


def bench(port, command, count, cpu):
    args = [command, "k:__rand_int__"]
    if command == "SET":
        args.append(VALUE)
    result = subprocess.run(
        ["taskset", "-c", cpu, "redis-benchmark", "-p", str(port), "-c", "64",
         "-n", str(count), "-r", str(KEY_COUNT), "-P", "1", "-k", "1",
         "-e", "--precision", "3", *args],
        env={**os.environ, "LC_ALL": "C"},
        capture_output=True, text=True, check=True, timeout=120,
    )
    if re.search(r"^ERROR:", result.stdout, re.M):
        raise RuntimeError(result.stdout[-3000:])
    qps = re.search(r"([\d.]+) requests per second", result.stdout)
    modern = re.search(r"^\s*avg\s+min\s+p50\s+p95\s+p99\s+max\s*\n\s*([\d. ]+)",
                       result.stdout, re.M)
    if modern:
        _, _, p50, _, p99, _ = map(float, modern[1].split())
    else:
        buckets = [(float(p), float(t)) for p, t in
                   re.findall(r"([\d.]+)% <= ([\d.]+) milliseconds", result.stdout)]
        p50 = next(t for p, t in buckets if p >= 50)
        p99 = next(t for p, t in buckets if p >= 99)
    if not qps:
        raise RuntimeError(result.stdout[-3000:] + result.stderr)
    return float(qps[1]), p50, p99


def trial(binary, network, count, server_cpu, client_cpu):
    port = free_port()
    with tempfile.TemporaryFile() as log:
        server = subprocess.Popen(
            ["taskset", "-c", server_cpu, binary, str(port), "hash",
             "--network", network, "--persistence", "none", "--role", "standalone"],
            stdout=log, stderr=log,
        )
        try:
            eventually(lambda: server.poll() is None and request(port, "PING") == b"PONG", 5)
            with socket.create_connection(("127.0.0.1", port), timeout=5) as conn:
                with conn.makefile("rb") as stream:
                    for start in range(0, KEY_COUNT, 8):
                        conn.sendall(b"".join(resp("SET", f"k:{key:012d}", VALUE)
                                              for key in range(start, start + 8)))
                        assert stream.read(40) == b"+OK\r\n" * 8
            assert request(port, "DBSIZE") == KEY_COUNT
            for command in ("SET", "GET"):
                bench(port, command, 10000, client_cpu)
            timings = {command: bench(port, command, count, client_cpu)
                       for command in ("SET", "GET")}
            assert request(port, "GET", "k:000000000001") == VALUE.encode()
            assert request(port, "DBSIZE") == KEY_COUNT
            status = Path(f"/proc/{server.pid}/status").read_text()
            rss = int(re.search(r"^VmRSS:\s+(\d+)", status, re.M)[1])
            return timings, rss
        except Exception:
            log.seek(0)
            print(log.read().decode(errors="replace"))
            raise
        finally:
            server.terminate()
            try:
                server.wait(timeout=8)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
                raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cases", nargs="+", help="label:reactor|proactor:/path/to/kvstore")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--requests", type=int, default=50000)
    parser.add_argument("--server-cpu", default="2")
    parser.add_argument("--client-cpu", default="4")
    parser.add_argument("--csv", type=Path, required=True)
    args = parser.parse_args()
    cases = [case.split(":", 2) for case in args.cases]
    with args.csv.open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(("case", "backend", "round", "command", "qps", "p50_ms", "p99_ms", "rss_kib"))
        samples = {}
        for round_id in range(1, args.rounds + 1):
            order = cases if round_id % 2 else reversed(cases)
            for label, network, binary in order:
                timings, rss = trial(binary, network, args.requests, args.server_cpu, args.client_cpu)
                for command, metrics in timings.items():
                    writer.writerow((label, network, round_id, command, *metrics, rss))
                    samples.setdefault((label, network, command), []).append((*metrics, rss))
                output.flush()
                print(label, network, round_id, timings, rss, flush=True)
    print("Medians")
    for key, values in samples.items():
        print(*key, *(statistics.median(series) for series in zip(*values)))


if __name__ == "__main__":
    main()
