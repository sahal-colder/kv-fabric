#!/usr/bin/env python3
"""Serial, local performance audit. Never runs two GPU/SSD experiments together.

Usage: VLLM_BATCH_INVARIANT=1 python scripts/run_perf_attribution.py OUT
Build build-before with the diagnostic baseline source in OUT/before, and
build-after from the patched tree. See the audit report for CMake commands.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out", type=Path)
    ap.add_argument("--model", type=Path, required=True)
    ap.add_argument("--disk-dir", type=Path, required=True)
    ap.add_argument("--before", type=Path, required=True)
    ap.add_argument("--after", type=Path, required=True)
    ap.add_argument("--io-bench", type=Path, required=True)
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PYTHONPATH=str(ROOT / "scripts"), KVF_PERF_TRACE="1",
               VLLM_BATCH_INVARIANT="1", VLLM_NO_USAGE_STATS="1")
    tmp = ROOT / ".kvfabric-local/perf-attribution/nsys-tmp"
    tmp.mkdir(parents=True, exist_ok=True)
    env["NSYS_TMPDIR"] = str(tmp)
    commands = []

    def run(name, cmd, stderr=None):
        commands.append({"name": name, "command": list(map(str, cmd)), "start": time.time(),
                         "env": {k: env[k] for k in ("VLLM_BATCH_INVARIANT", "KVF_PERF_TRACE", "NSYS_TMPDIR")}})
        (out / "commands.json").write_text(json.dumps(commands, indent=2))
        print("START", name, flush=True)
        with (out / f"{name}.log").open("w") as f:
            if stderr:
                with stderr.open("w") as err:
                    result = subprocess.run(list(map(str, cmd)), env=env, stdout=f, stderr=err)
            else:
                result = subprocess.run(list(map(str, cmd)), env=env, stdout=f, stderr=subprocess.STDOUT)
        commands[-1].update(returncode=result.returncode, end=time.time())
        (out / "commands.json").write_text(json.dumps(commands, indent=2))
        if result.returncode:
            raise SystemExit(f"{name} failed: {result.returncode}; raw log preserved")
        print("PASS", name, flush=True)

    reference = out / "nsys-cpu/requests.json"
    for label, build in (("cpu", a.before), ("before-diagnostic", a.before), ("final-disk", a.after)):
        target = out / ("nsys-" + label)
        cmd = [
            "nsys", "profile", "--trace=cuda,nvtx,osrt", "--sample=none", "--cpuctxsw=none",
            "--capture-range=cudaProfilerApi", "--capture-range-end=stop",
            "--trace-fork-before-exec=true", "--cuda-flush-interval=0", "--osrt-threshold=1000",
            "-o", target / "trace", "python3", ROOT / "scripts/profile_vllm_kv.py",
            "optimized", build, a.model, target,
            "--requests", "3", "--warmup-requests", "3", "--profile"]
        if label != "cpu":
            cmd += ["--disk-dir", a.disk_dir, "--reference", reference]
        run("nsys-" + label, cmd)
        run("export-" + label, ["nsys", "export", "--type=sqlite", "--output",
            target / "trace.sqlite", target / "trace.nsys-rep"])
    # Unprofiled endpoints use the established benchmark and exact historical prompts.
    for label, build in (("before", a.before), ("final", a.after)):
        run("matrix-" + label, ["python3", ROOT / "scripts/bench_vllm_kv.py",
            "--model", a.model, "--lengths", "16384", "--shared", "0.5",
            "--modes", "kvfabric-disk", "--out", out / ("matrix-" + label),
            "--disk-dir", a.disk_dir, "--require-native-nvme", "--library",
            build / "src/integration/libkvf_vllm.so", "--directory", build / "bin/kvfabricd",
            "--cache-gib", "1.5", "--gpu-gib", "1", "--concurrency", "1",
            "--requests", "15", "--output-tokens", "16", "--warmup-requests", "3",
            "--expected-io-mode", "direct-uring-fixed", "--require-cache-write-success", "--reference",
            ROOT / "docs/benchmarks/native-linux-final/matrix-c1/recompute/requests.jsonl"])
    # Same per-file eviction, no global cache drops. Report cold start and hot-cache mix.
    for repeat, modes in enumerate((("fixed", "direct", "buffered"), ("buffered", "direct", "fixed"))):
        for lanes in (1, 4):
            for mode in modes:
                name = f"io-r{repeat}-c{lanes}-{mode}"
                run(name, [a.io_bench, a.disk_dir, mode, lanes, 200], out / (name + ".json"))
    run("ctest-after", ["ctest", "--test-dir", a.after, "--output-on-failure"])
    run("owned-disk-parallel", ["python3", ROOT / "tests/vllm/test_owned_disk_parallel.py",
                               a.after, out / "owned-disk-parallel"])


if __name__ == "__main__":
    main()
