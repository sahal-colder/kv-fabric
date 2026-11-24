"""Measure fresh, interleaved baseline and candidate processes with the Nano test workload."""

import argparse
import contextlib
import csv
from datetime import datetime
import hashlib
import io
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile

EXPECTED_MODEL = "cd590644d963867a2b6e5a1107f51fad663c41d79c149fbecbbb1f95fa81f49a"
EXPECTED_TOKENIZER = "50a52ef822ee9e83de5ce9d0be0a025a773d019437f58b5ff9dcafb063ece361"
TOKEN_IDS = "297 278 4799 29889 2178 278 916 15006"
MODES = ("cold", "local", "remote")
LIMITS = (("p50", 3), ("p95", 5), ("p99", 5))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_csv(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def environment(args):
    lines = [
        f"Timestamp = {datetime.now().astimezone().isoformat()}",
        f"System = {platform.platform()}",
        f"Python = {platform.python_version()}",
        f"CPU affinity = {sorted(os.sched_getaffinity(0))}",
        f"Model SHA-256 = {EXPECTED_MODEL}",
        f"Tokenizer SHA-256 = {EXPECTED_TOKENIZER}",
        f"Rounds = {args.rounds}",
        "Topology = IPv4 TCP 127.0.0.1, independent directory, publisher and consumer processes",
        "Order = baseline-first on odd rounds, after-first on even rounds, one process group at a time",
        "Cold/Local/Remote order rotates by round. Each group gets one producer prime.",
        "One unrelated warmup per consumer. Local also primes the target prefix.",
        "Prompt = The quick brown fox repeated four times, then walks away (prime) or jumps high (target)",
        "Sampling = 8 tokens, seed 1234, temperature 0.7, top-p 0.95, ignore EOS",
        "Batch=4, budget=16, pages=48, page size=4, exports=48, timeout=2000 ms, TP=1, PP=1",
        "TTFT = before prompt encoding to first completed sample. OpenBLAS and OMP threads = 1",
        "Quantiles = nearest rank. Delta = (after / baseline - 1) * 100.",
    ]
    commands = [
        ("Compiler", ["gcc", "--version"]),
        ("CMake", ["cmake", "--version"]),
        ("OpenBLAS, liburing, OpenSSL", ["pkg-config", "--modversion", "openblas", "liburing", "openssl"]),
    ]
    if args.device == "cuda":
        commands += [
            ("GPU", ["nvidia-smi", "--format=csv", "--query-gpu=name,driver_version,memory.total"]),
            ("CUDA Toolkit", ["nvcc", "--version"]),
        ]
    for name, cmd in commands:
        lines.append(f"{name} = {subprocess.check_output(cmd, text=True).strip()}")
    for variant in ("baseline", "after"):
        path = args.before if variant == "baseline" else args.after
        lines.append(f"{variant} build = {path}")
        lines.extend(line for line in (path / "CMakeCache.txt").read_text().splitlines() if line.startswith(
            ("CMAKE_BUILD_TYPE:", "CMAKE_C_COMPILER:", "CMAKE_CUDA_COMPILER:", "CMAKE_CUDA_ARCHITECTURES:",
             "NANO_WITH_CUDA:", "MINIINFER_WITH_CUDA:", "MINIINFER_WITH_URING:")))
        for name in ("kvfabricd", "test_nano_remote_worker"):
            lines.append(f"{name} SHA-256 = {digest(path / 'bin' / name)}")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ("before", "after", "model", "tokenizer", "out", "harness"):
        parser.add_argument(f"--{field}", type=Path, required=True)
    parser.add_argument("--device", choices=("cpu", "cuda"), required=True)
    parser.add_argument("--rounds", type=int, default=100)
    args = parser.parse_args()
    if not __debug__ or args.rounds < 30:
        parser.error("enable assertions and use at least 30 paired rounds")
    if digest(args.model) != EXPECTED_MODEL or digest(args.tokenizer) != EXPECTED_TOKENIZER:
        parser.error("supply the fixed stories15M checkpoint and tokenizer")
    sys.path.insert(0, str(args.harness.resolve()))
    from test_remote_inference import Runs, benchmark, percentiles, write_csv

    baseline = args.out / "baseline"
    after = args.out / "after"
    for path in (baseline, after):
        path.mkdir(parents=True, exist_ok=True)
    with (after / f"{args.device}-environment.txt").open("x", encoding="utf-8") as stream:
        stream.write(environment(args))
    builds = (("baseline", args.before), ("after", args.after))
    samples = {"baseline": [], "after": []}
    staging = {"baseline": [], "after": []}
    paired = []
    for sample in range(args.rounds):
        order = builds if sample % 2 == 0 else builds[::-1]
        for variant, path in order:
            with tempfile.TemporaryDirectory(prefix="kvf-pair-") as temporary:
                root = Path(temporary)
                raw = root / "raw.csv"
                config = argparse.Namespace(model=args.model, tokenizer=args.tokenizer, device=args.device,
                                            directory=path / "bin/kvfabricd", worker=path / "bin/test_nano_remote_worker")
                with contextlib.redirect_stdout(io.StringIO()):
                    benchmark(Runs(config, root), raw, 1, warmup=1, offset=sample)
                samples[variant].extend(read_csv(raw))
                staging[variant].extend(read_csv(root / "raw-transfers.csv"))
        b = {row["mode"]: row for row in samples["baseline"][-3:]}
        a = {row["mode"]: row for row in samples["after"][-3:]}
        for mode in MODES:
            for row in (b[mode], a[mode]):
                remote = mode == "remote"
                assert row["sample"] == str(sample + 1) and row["generated_token_ids"] == TOKEN_IDS, row
                assert int(row["prompt_tokens"]) == 24 and int(row["prefilled"]) == (24 if mode == "cold" else 4), row
                assert int(row["local"]) == (20 if mode == "local" else 0), row
                assert int(row["remote_prefill_skipped_tokens"]) == (20 if remote else 0), row
                assert int(row["data_requests"]) == int(row["remote_lookup_requests"]) == int(remote), row
                assert int(row["received_bytes"]) == int(row["remote_import_bytes"]) == (276480 if remote else 0), row
                assert int(row["remote_prefetch_failures"]) == int(row["remote_admission_skips"]) == 0, row
                assert (int(row["cuda_h2d_ns"]) > 0) == (args.device == "cuda" and remote), row
            first, last = int(b[mode]["ttft_ns"]), int(a[mode]["ttft_ns"])
            paired.append(dict(device=args.device, sample=sample+1, first=order[0][0], mode=mode,
                               baseline_ns=first, after_ns=last, delta_ns=last-first,
                               delta_percent=(last/first-1)*100))
        if (sample + 1) % 10 == 0 or sample + 1 == args.rounds:
            print(f"{args.device}: {sample + 1}/{args.rounds} paired rounds", flush=True)
    for variant, path in (("baseline", baseline), ("after", after)):
        assert len(samples[variant]) == 3 * args.rounds and len(staging[variant]) == 5 * args.rounds
        write_csv(path / f"{args.device}-raw.csv", samples[variant])
        write_csv(path / f"{args.device}-page-staging.csv", staging[variant])
    write_csv(after / f"{args.device}-paired-ttft.csv", paired)
    summary = []
    for mode in MODES:
        group = [row for row in paired if row["mode"] == mode]
        before = percentiles(row["baseline_ns"] for row in group)
        candidate = percentiles(row["after_ns"] for row in group)
        median = statistics.median(row["delta_percent"] for row in group)
        median_ns = statistics.median(row["delta_ns"] for row in group)
        for (quantile, limit), b, a in zip(LIMITS, before, candidate):
            delta = (a / b - 1) * 100
            summary.append(dict(device=args.device, mode=mode, quantile=quantile, rounds=args.rounds, baseline_ns=b,
                                after_ns=a, delta_percent=delta, limit_percent=limit, passed=delta <= limit,
                                paired_median_delta_ns=median_ns, paired_median_delta_percent=median))
            print(f"{args.device} {mode} {quantile}: {b/1e6:.3f} -> {a/1e6:.3f} ms ({delta:+.2f}%)")
    write_csv(after / f"{args.device}-ttft-summary.csv", summary)
    return 0 if all(row["passed"] for row in summary) else 2


if __name__ == "__main__":
    raise SystemExit(main())
