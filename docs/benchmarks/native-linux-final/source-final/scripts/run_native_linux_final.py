#!/usr/bin/env python3
"""Run the current archived native-Linux plan in its isolated vLLM environment.

Environment/build preparation precedes this driver. A failed regression stops
all dependent work. Every child command, return code and service log is retained.
"""
import argparse
from datetime import datetime
import importlib.metadata
import json
import os
from pathlib import Path
import subprocess
import sys
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "docs/benchmarks/native-linux-final"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("regression", "matrix", "profile", "all"))
    parser.add_argument("--resume", action="store_true", help="retry after archiving the failed partial mode")
    args = parser.parse_args()
    run = json.loads((OUT / "run.json").read_text())
    plan = json.loads((OUT / "planned-matrix.json").read_text())
    build, posix = Path(run["build"]), Path(run["build_posix"])
    venv = Path(run["work"]) / "venv"
    if Path(sys.prefix).resolve() != venv.resolve():
        raise SystemExit(f"Run with {venv}/bin/python")
    if importlib.metadata.version("vllm") != "0.31.0":
        raise SystemExit("This connector requires the repository-pinned vLLM 0.31.0")
    env = dict(os.environ, PATH=str(venv / "bin") + os.pathsep + os.environ["PATH"],
               PYTHONDONTWRITEBYTECODE="1", VLLM_NO_USAGE_STATS="1",
               HF_HUB_OFFLINE="1", TRANSFORMERS_OFFLINE="1", VLLM_BATCH_INVARIANT="1")
    for key, folder in (("XDG_CACHE_HOME", "cache"), ("TRITON_CACHE_DIR", "cache/triton"),
                        ("VLLM_CACHE_ROOT", "cache/vllm"), ("CUDA_CACHE_PATH", "cache/cuda"),
                        ("TORCHINDUCTOR_CACHE_DIR", "cache/torchinductor")):
        env[key] = str(Path(run["work"]) / folder)
    state_file = OUT / "execution-status.json"
    state = json.loads(state_file.read_text()) if state_file.exists() else {}

    def execute(name, command):
        if state.get(name, {}).get("exit") == 0:
            print(f"{name}: previously completed in this run", flush=True)
            return
        if name in state and not args.resume:
            raise RuntimeError(f"{name} already failed; inspect its logs and use a new output before retry")
        previous = state.get(name)
        if previous:
            stamp = datetime.now(ZoneInfo("Australia/Perth")).strftime("%Y%m%dT%H%M%S")
            for suffix in (".log", "-gpu-telemetry.csv"):
                old = OUT / f"{name}{suffix}"
                if old.exists():
                    old.rename(OUT / f"{name}-failed-{stamp}{suffix}")
        started = datetime.now(ZoneInfo("Australia/Perth")).isoformat()
        print(f"START {name}: {started}", flush=True)
        with (OUT / f"{name}.log").open("w") as log:
            log.write("COMMAND " + json.dumps(list(map(str, command))) + "\n")
            log.flush()
            with (OUT / f"{name}-gpu-telemetry.csv").open("w") as telemetry:
                monitor = subprocess.Popen(["nvidia-smi", "--query-gpu=timestamp,name,pstate,temperature.gpu,"
                    "power.draw,memory.used,utilization.gpu,clocks.sm,clocks.mem", "--format=csv", "-l", "1"],
                    stdout=telemetry, stderr=subprocess.STDOUT)
                try:
                    result = subprocess.run(list(map(str, command)), env=env, cwd=ROOT,
                                            stdout=log, stderr=subprocess.STDOUT)
                finally:
                    monitor.terminate()
                    monitor.wait()
        record = {"command": list(map(str, command)), "started": started,
                  "finished": datetime.now(ZoneInfo("Australia/Perth")).isoformat(),
                  "exit": result.returncode}
        if previous:
            record["prior_attempt"] = previous
        state[name] = record
        state_file.write_text(json.dumps(state, indent=2))
        with (OUT / "commands.jsonl").open("a") as stream:
            stream.write(json.dumps({"name": name, **record}) + "\n")
        print(f"END {name}: exit={result.returncode}", flush=True)
        if result.returncode:
            print((OUT / f"{name}.log").read_text()[-6000:], flush=True)
            raise SystemExit(result.returncode)

    if args.phase in ("regression", "all"):
        execute("connector-roundtrip", [sys.executable, ROOT / "tests/vllm/test_connector_roundtrip.py",
            "--model", plan["model"], "--library", build / "src/integration/libkvf_vllm.so",
            "--directory", build / "bin/kvfabricd", "--require-uring-fixed",
            "--out", OUT / "connector-roundtrip"])
    if args.phase in ("matrix", "profile", "all"):
        if state.get("connector-roundtrip", {}).get("exit") != 0:
            raise SystemExit("Full connector regression must pass before the matrix/profiles")
    if args.phase in ("matrix", "all"):
        def bench(build_dir, destination, lengths, concurrency, modes, *extra):
            return [sys.executable, ROOT / "scripts/bench_vllm_kv.py", "--model", plan["model"],
                "--library", build_dir / "src/integration/libkvf_vllm.so",
                "--directory", build_dir / "bin/kvfabricd", "--disk-dir", Path(run["work"]) / "disk",
                "--lengths", *map(str, lengths), "--shared", "0.5", "--requests", "15",
                "--output-tokens", "16", "--warmup-requests", "3", "--concurrency", str(concurrency),
                "--gpu-gib", "1", "--cache-gib", "1.5", "--out", destination, "--modes", *modes, *extra,
                *(["--resume"] if args.resume else [])]
        modes = ["recompute", "native", "kvfabric-cpu", "kvfabric-disk"]
        execute("matrix-c1", bench(build, OUT / "matrix-c1", plan["lengths"], 1, modes,
            "--require-native-nvme", "--expected-io-mode", "direct-uring-fixed"))
        reference = OUT / "matrix-c1/recompute/requests.jsonl"
        execute("matrix-native-external-c1", bench(build, OUT / "matrix-native-external-c1", plan["lengths"],
            1, ["native"], "--reset-gpu-between-batches", "--reference", reference))
        execute("matrix-posix-c1", bench(posix, OUT / "matrix-posix-c1", plan["lengths"], 1,
            ["kvfabric-disk"], "--require-native-nvme", "--expected-io-mode", "direct-posix",
            "--reference", reference))
        execute("matrix-legacy-c1", bench(build, OUT / "matrix-legacy-c1", plan["lengths"], 1,
            ["kvfabric-cpu"], "--legacy-cuda", "--reference", reference))
        for concurrency in plan["concurrencies"]:
            if concurrency == 1:
                continue
            execute(f"matrix-c{concurrency}", bench(build, OUT / f"matrix-c{concurrency}",
                [8192], concurrency, modes, "--require-native-nvme", "--expected-io-mode", "direct-uring-fixed"))
            execute(f"matrix-posix-c{concurrency}", bench(posix, OUT / f"matrix-posix-c{concurrency}",
                [8192], concurrency, ["kvfabric-disk"], "--require-native-nvme", "--expected-io-mode",
                "direct-posix", "--reference", OUT / f"matrix-c{concurrency}/recompute/requests.jsonl"))
            execute(f"matrix-native-external-c{concurrency}", bench(build,
                OUT / f"matrix-native-external-c{concurrency}", [8192], concurrency, ["native"],
                "--reset-gpu-between-batches", "--reference", OUT / f"matrix-c{concurrency}/recompute/requests.jsonl"))
    if args.phase in ("profile", "all"):
        for mode in ("baseline", "optimized"):
            execute(f"nsys-{mode}", ["bash", ROOT / "scripts/profile_kv.sh", mode, build,
                                      plan["model"], OUT / f"nsys-{mode}"])


if __name__ == "__main__":
    main()
