"""Run the fixed Nano-VLLM KV benchmark in bounded, interleaved shards."""

import argparse
import csv
import hashlib
from pathlib import Path
import math
import os
import platform
import random
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / "tests/nanovllm/test_remote_inference.py"
MODES = ("cold", "local", "remote")
STAGES = (
    "prefetch_queue_ns", "dir_lookup_ns", "conn_acquire_ns", "net_recv_ns",
    "crc_verify_ns", "cpu_stage_ns", "cuda_h2d_ns", "cuda_sync_ns", "lease_check_ns",
    "kv_commit_ns", "sched_resume_ns", "first_decode_ns",
)
METRICS = STAGES + (
    "ttft_ns", "prefill_ns", "decode_ns", "decode_tokens_per_s",
    "received_bytes", "data_requests", "remote_import_bytes", "remote_lookup_requests",
    "remote_prefetch_requests", "remote_prefetch_failures", "remote_admission_skips",
    "cpu_ns", "rss_kib", "gpu_mib", "gpu_device_delta_bytes", "publisher_cpu_ns",
    "publisher_rss_kib", "publisher_gpu_mib", "publisher_gpu_device_delta_bytes",
    "directory_cpu_ns", "directory_rss_kib",
)
CHECKPOINT = "cd590644d963867a2b6e5a1107f51fad663c41d79c149fbecbbb1f95fa81f49a"
TOKENIZER = "50a52ef822ee9e83de5ce9d0be0a025a773d019437f58b5ff9dcafb063ece361"


def digest(file):
    sha = hashlib.sha256()
    with file.open("rb") as input_file:
        for chunk in iter(lambda: input_file.read(1024 * 1024), b""):
            sha.update(chunk)
    return sha.hexdigest()


def command_text(*cmd):
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=8, check=True)
        return result.stdout.strip()
    except (OSError, subprocess.SubprocessError) as error:
        return f"unavailable ({error})"


def environment(args):
    lines = [
        "Topology = one WSL2 guest, separate processes, IPv4 TCP 127.0.0.1",
        "TTFT starts before prompt encoding and ends at the first completed sample.",
        f"Samples per mode = {args.samples}",
        f"Rounds per shard = {args.shard_size}",
        f"Unrelated warmup requests per measured consumer = {args.warmup}",
        "Producer prewarms the target prefix once per shard. Local consumers prime it separately.",
        "Checkpoint = stories15M.bin",
        f"Checkpoint SHA256 = {CHECKPOINT}",
        f"Tokenizer SHA256 = {TOKENIZER}",
        "Inputs = The quick brown fox repeated four times, then walks away (prime) or jumps high (sample)",
        "Batch=4, budget=16, pages=48, page size=4, exports=48, timeout=2000 ms, TP=1, PP=1",
        "Sampling = 8 generated tokens, seed 1234, temperature 0.7, top-p 0.95, ignore EOS",
        "OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1",
        f"Percentiles = nearest rank, paired bootstrap = 5000 resamples, RNG seed 1234",
        f"Git base = {command_text('git', '-C', str(ROOT), 'rev-parse', 'HEAD')}",
        f"Python = {platform.python_version()}",
        f"Linux = {command_text('uname', '-srvm')}",
        Path("/etc/os-release").read_text().strip(),
        command_text("lscpu"),
        next(line.strip() for line in Path("/proc/meminfo").read_text().splitlines() if line.startswith("MemTotal:")),
        command_text("nvidia-smi", "--query-gpu=name,driver_version,memory.total", "--format=csv"),
        command_text("nvcc", "--version"),
        command_text("gcc", "--version"),
        command_text("cmake", "--version"),
        "OpenBLAS / liburing / OpenSSL versions",
        command_text("pkg-config", "--modversion", "openblas", "liburing", "openssl"),
    ]
    for entry in args.build:
        device, path = entry.split("=", 1)
        build = Path(path).resolve()
        cache = build / "CMakeCache.txt"
        options = cache.read_text().splitlines()
        lines.extend([
            f"{device.upper()} build = {build.name}",
            *(line for line in options if line.startswith(
                ("CMAKE_BUILD_TYPE:", "NANO_WITH_CUDA:", "MINIINFER_WITH_CUDA:", "MINIINFER_WITH_URING:",
                 "MINIINFER_WITH_SPDK:", "CMAKE_CUDA_ARCHITECTURES:", "CMAKE_C_COMPILER:"))),
            f"Worker binary SHA256 = {digest(build / 'bin/test_nano_remote_worker')}",
            f"Directory binary SHA256 = {digest(build / 'bin/kvfabricd')}",
        ])
    return "\n".join(lines) + "\n"


def read_csv(file):
    with file.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def write_csv(file, rows):
    with file.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0])
        writer.writeheader()
        writer.writerows(rows)


def ranked(values, fraction):
    return sorted(values)[math.ceil(fraction * len(values)) - 1]


def run(device, build, args, output):
    rows, pages = [], []
    with tempfile.TemporaryDirectory(prefix=f"kvf-{device}-shards-") as temporary:
        for offset in range(0, args.samples, args.shard_size):
            count = min(args.samples - offset, args.shard_size)
            shard = Path(temporary) / "samples.csv"
            print(f"{device}: measuring rounds {offset + 1}-{offset + count}", flush=True)
            call = [
                sys.executable, str(HARNESS), "--device", device, "--benchmark", str(shard),
                "--directory", str(build / "bin/kvfabricd"),
                "--worker", str(build / "bin/test_nano_remote_worker"),
                "--model", str(args.model), "--tokenizer", str(args.tokenizer),
                "--samples", str(count), "--sample-offset", str(offset), "--warmup", str(args.warmup),
            ]
            subprocess.run(call, env=dict(os.environ, OPENBLAS_NUM_THREADS="1", OMP_NUM_THREADS="1"),
                           check=True, timeout=args.shard_timeout, stdout=subprocess.DEVNULL)
            rows.extend(read_csv(shard))
            pages.extend(read_csv(shard.with_name("samples-transfers.csv")))
    write_csv(output / f"{device}-raw.csv", rows)
    write_csv(output / f"{device}-page-staging.csv", pages)
    assert len(rows) == 3 * args.samples and len(pages) == 5 * args.samples
    assert len({row["generated_token_ids"] for row in rows}) == 1
    groups = {mode: {} for mode in MODES}
    page_totals = {}
    for page in pages:
        sample = int(page["sample"])
        page_totals.setdefault(sample, []).append((int(page["page_in_transfer"]), int(page["transfer_ns"])))
    for row in rows:
        assert row["device"] == device and row["loopback"] == "127.0.0.1", row
        mode, sample = row["mode"], int(row["sample"])
        assert sample not in groups[mode], row
        groups[mode][sample] = row
        reused = 20 if mode == "remote" else 0
        assert int(row["prompt_tokens"]) == 24 and int(row["remote_prefill_skipped_tokens"]) == reused, row
        assert int(row["local"]) == (20 if mode == "local" else 0), row
        assert int(row["prefilled"]) == (24 if mode == "cold" else 4), row
        assert int(row["remote_import_bytes"]) == int(row["received_bytes"]) == (276480 if reused else 0), row
        assert int(row["remote_lookup_requests"]) == int(row["data_requests"]) == int(bool(reused)), row
        assert int(row["remote_prefetch_requests"]) == int(bool(reused)), row
        assert int(row["remote_prefetch_failures"]) == int(row["remote_admission_skips"]) == 0, row
        assert int(row["ttft_ns"]) >= sum(int(row[field]) for field in STAGES), row
        assert (int(row["cuda_h2d_ns"]) > 0) == (device == "cuda" and mode == "remote"), row
        if reused:
            samples = sorted(page_totals[sample])
            assert [n for n, _ in samples] == list(range(1, 6)), page_totals[sample]
            assert sum(ns for _, ns in samples) == int(row["remote_transfer_latency_ns"]), row

    summary = []
    for mode, group in groups.items():
        assert set(group) == set(range(1, args.samples + 1))
        for metric in METRICS:
            number = float if metric == "decode_tokens_per_s" else int
            values = [number(row[metric]) for row in group.values() if row[metric]]
            if values:
                summary.append(dict(mode=mode, metric=metric, samples=len(values), total=sum(values),
                                    p50=ranked(values, .5), p95=ranked(values, .95), p99=ranked(values, .99)))
    write_csv(output / f"{device}-summary.csv", summary)
    deltas = [int(groups["cold"][n]["ttft_ns"]) - int(groups["remote"][n]["ttft_ns"])
              for n in range(1, args.samples + 1)]
    rng = random.Random(1234)
    distribution = [ranked(rng.choices(deltas, k=len(deltas)), .5) for _ in range(5000)]
    ttft = {row["mode"]: row["p50"] for row in summary if row["metric"] == "ttft_ns"}
    paired = dict(device=device, samples=len(deltas), cold_p50_ns=ttft["cold"], remote_p50_ns=ttft["remote"],
                  faster_pairs=sum(d > 0 for d in deltas), paired_median_delta_ns=ranked(deltas, .5),
                  bootstrap_low_ns=ranked(distribution, .025), bootstrap_high_ns=ranked(distribution, .975))
    print(paired, flush=True)
    return paired


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="append", required=True, metavar="cpu|cuda=BUILD_DIR")
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--tokenizer", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=30)
    parser.add_argument("--shard-size", type=int, default=10)
    parser.add_argument("--shard-timeout", type=int, default=120)
    parser.add_argument("--warmup", type=int, default=1)
    args = parser.parse_args()
    if not __debug__ or args.samples < 30 or args.shard_size < 1 or args.shard_timeout < 1 or args.warmup < 0:
        parser.error("enable assertions and use at least 30 rounds with positive shard size and timeout")
    builds = [entry.partition("=") for entry in args.build]
    if any(not sep or device not in ("cpu", "cuda") or not path for device, sep, path in builds):
        parser.error("use --build cpu=BUILD_DIR or --build cuda=BUILD_DIR")
    if len({device for device, _, _ in builds}) != len(builds):
        parser.error("supply one build per device")
    if digest(args.model) != CHECKPOINT or digest(args.tokenizer) != TOKENIZER:
        parser.error("checkpoint and tokenizer must match the historical stories15M workload")
    args.out.mkdir(parents=True, exist_ok=False)
    (args.out / "environment.txt").write_text(environment(args), encoding="utf-8")
    paired = [run(device, Path(path).resolve(), args, args.out) for device, _, path in builds]
    write_csv(args.out / "paired-ttft.csv", paired)
    return int(any(row["device"] == "cuda" and
                   (row["remote_p50_ns"] >= row["cold_p50_ns"] or row["bootstrap_low_ns"] <= 0)
                   for row in paired))


if __name__ == "__main__":
    raise SystemExit(main())
