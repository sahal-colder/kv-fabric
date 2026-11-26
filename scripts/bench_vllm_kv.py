#!/usr/bin/env python3
"""Replay the same token-ID workload against each vLLM KV cache configuration."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager, ExitStack
import importlib.metadata
import hashlib
import json
import math
import os
from pathlib import Path
import random
import re
import shlex
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MODES = ("recompute", "native", "kvfabric-cpu", "kvfabric-disk", "lmcache-cpu", "lmcache-disk")
GIB = 1024 ** 3
HTTP = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


@contextmanager
def process(command, env, log):
    with log.open("w", encoding="utf-8") as stream:
        stream.write(shlex.join(map(str, command)) + "\n")
        stream.flush()
        child = subprocess.Popen(command, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                 start_new_session=True)
        try:
            yield child
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGTERM)
                try:
                    child.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait()


def await_ready(child, log, check, timeout=180):
    deadline = time.monotonic() + timeout
    while child.poll() is None and time.monotonic() < deadline:
        try:
            if check():
                return
        except OSError:
            pass
        time.sleep(0.25)
    raise RuntimeError(f"Process failed to become ready: {log}\n{log.read_text()[-9000:]}")


def get(url, timeout=2):
    with HTTP.open(url, timeout=timeout) as response:
        return response.read()


@contextmanager
def directory(binary, log, pages=8192):
    port = free_port()
    config = log.with_suffix(".conf")
    config.write_text(f"listen = 127.0.0.1:{port}\nmax_pages = {pages}\n")
    with process([binary, "--config", config], os.environ.copy(), log) as child:
        await_ready(child, log, lambda: "READY " in log.read_text(), timeout=10)
        yield f"127.0.0.1:{port}"


@contextmanager
def server(model, log, length, gpu_bytes, concurrency, options=(), env=None):
    port = free_port()
    url = f"http://127.0.0.1:{port}"
    child_env = dict(os.environ, VLLM_WORKER_MULTIPROC_METHOD="spawn", VLLM_SERVER_DEV_MODE="1")
    plugin = str(ROOT / "integrations/vllm")
    child_env["PYTHONPATH"] = os.pathsep.join(filter(None, [plugin, child_env.get("PYTHONPATH")]))
    child_env.update(env or {})
    command = [
        "vllm", "serve", model, "--host", "127.0.0.1", "--port", str(port),
        "--served-model-name", "kvf-bench", "--dtype", "bfloat16", "--block-size", "16",
        "--attention-backend", "FLASH_ATTN", "--max-model-len", str(length),
        "--kv-cache-memory-bytes", str(gpu_bytes), "--max-num-seqs", str(concurrency),
        "--max-num-batched-tokens", "2048", "--enforce-eager", "--generation-config", "vllm",
        "--shutdown-timeout", "10",
        *options,
    ]
    with process(command, child_env, log) as child:
        await_ready(child, log, lambda: get(f"{url}/health") == b"", timeout=600)
        yield url


def transfer_config(connector, extra, module=None):
    values = {"kv_connector": connector, "kv_role": "kv_both", "kv_load_failure_policy": "recompute",
              "kv_connector_extra_config": extra}
    if module:
        values["kv_connector_module_path"] = module
    return ["--kv-transfer-config", json.dumps(values)]


def kvfabric_config(extra):
    return transfer_config("KVFabricConnector", extra, "kvfabric_vllm.connector")


def generate(url, ids, new, seed=123):
    body = json.dumps({
        "model": "kvf-bench", "prompt": ids, "max_tokens": new, "temperature": 0,
        "ignore_eos": True, "seed": seed, "stream": True, "return_token_ids": True,
        "stream_options": {"include_usage": True},
    }).encode()
    request = urllib.request.Request(f"{url}/v1/completions", body, {"Content-Type": "application/json"})
    start = time.monotonic_ns()
    stamps, output, chunks = [], [], []
    request_id = None
    with HTTP.open(request, timeout=180) as response:
        for line in response:
            if not line.startswith(b"data: ") or line.startswith(b"data: [DONE]"):
                continue
            event = json.loads(line[6:])
            request_id = event["id"]
            for choice in event["choices"]:
                tokens = choice.get("token_ids") or []
                if tokens:
                    stamps.append(time.monotonic_ns())
                    chunks.append(len(tokens))
                    output.extend(tokens)
    if len(output) != new:
        raise RuntimeError(f"{request_id}: expected {new} output IDs, got {len(output)}")
    return {
        "request_id": request_id, "prompt_tokens": len(ids), "output_ids": output,
        "prompt_sha256": hashlib.sha256(json.dumps(ids, separators=(",", ":")).encode()).hexdigest(),
        "ttft_ms": (stamps[0] - start) / 1e6, "tpot_ms": (stamps[-1] - stamps[0]) / (new - 1) / 1e6
        if new > 1 else None, "itl_ms": [(b - a) / 1e6 for a, b in zip(stamps, stamps[1:])],
        "output_chunk_sizes": chunks,
    }


def reset_gpu_cache(url):
    for _ in range(100):
        req = urllib.request.Request(f"{url}/reset_prefix_cache", method="POST")
        with HTTP.open(req, timeout=10) as response:
            if json.load(response)["success"]:
                return
        time.sleep(0.05)
    raise RuntimeError("vLLM GPU prefix reset did not release blocks")


def token_sources(text):
    counters = {}
    for line in text.splitlines():
        if line.startswith("vllm:prompt_tokens_by_source_total{"):
            label = re.search(r'\bsource="([^"]+)"', line).group(1)
            counters[label] = counters.get(label, 0) + int(float(line.split()[-1]))
    return counters


def lmcache_counters(url):
    counters = {}
    for line in get(f"{url}/metrics").decode().splitlines():
        if line.startswith(("lmcache_mp_lookup_hit_l", "lmcache_mp_l2_prefetch_load_completed",
                            "lmcache_mp_l2_store_completed_objects")):
            key = line.split("{", 1)[0].split()[0]
            counters[key] = counters.get(key, 0) + int(float(line.split()[-1]))
    return counters


def lmcache_disk_objects(counters):
    return sum(n for name, n in counters.items() if "_l2_store_completed_objects_" in name)


def clear_lmcache_l1(url):
    with HTTP.open(urllib.request.Request(f"{url}/cache/clear", method="POST"), timeout=5) as response:
        if json.load(response).get("status") != "ok":
            raise RuntimeError("LMCache could not clear the L1 cache")


def prime_lmcache_disk(url, before, minimum_chunks):
    deadline = time.monotonic() + 20
    while lmcache_disk_objects(lmcache_counters(url)) - before < minimum_chunks:
        if time.monotonic() >= deadline:
            raise RuntimeError("LMCache did not commit the primed prefix to disk")
        time.sleep(0.1)
    clear_lmcache_l1(url)


def percentile(values):
    ordered = sorted(values)
    return {f"p{p}": ordered[math.ceil(p * len(ordered) / 100) - 1] if ordered else None
            for p in (50, 95, 99)}


def model_path(args):
    if args.model:
        return args.model
    from huggingface_hub import snapshot_download

    for name in ("Qwen/Qwen3-4B", "Qwen/Qwen2.5-3B-Instruct"):
        try:
            path = Path(snapshot_download(name, local_files_only=True))
        except OSError:
            continue
        index = path / "model.safetensors.index.json"
        if index.is_file():
            shards = set(json.loads(index.read_text())["weight_map"].values())
            if shards and all((path / shard).is_file() for shard in shards):
                return str(path)
        elif (path / "model.safetensors").is_file():
            return str(path)
    if args.fallback_model:
        return args.fallback_model
    raise SystemExit("No cached Qwen3-4B or Qwen2.5-3B-Instruct weights; pass --model or --fallback-model")


def read_events(path):
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def wait_kvf_publication(path, request_id, tokens, timeout=40):
    deadline = time.monotonic() + timeout
    while True:
        events = [event for event in read_events(path)
                  if event.get("request_id", "").startswith(request_id + "-")]
        if sum(event["tokens"] for event in events if event["event"] == "stored") >= tokens:
            return
        if any(event["event"] in ("save_failure", "save_skipped") for event in events):
            raise RuntimeError(f"KV-Fabric could not publish the complete prefix for {request_id}")
        if time.monotonic() >= deadline:
            raise RuntimeError(f"KV-Fabric publication timed out for {request_id}")
        time.sleep(0.05)


def wait_kvf_idle(path, timeout=90):
    """Drain only between groups/warmups, never between measured requests."""
    deadline = time.monotonic() + timeout
    while True:
        events = read_events(path)
        pending = sum(e["event"] == "save_queued" for e in events) - sum(
            e["event"] == "save_complete" for e in events)
        if not pending:
            return
        if pending < 0 or time.monotonic() >= deadline:
            raise RuntimeError(f"KV-Fabric publication queue did not drain: pending={pending}")
        time.sleep(.05)


def benchmark(args):
    from transformers import AutoConfig

    model = model_path(args)
    config = AutoConfig.from_pretrained(model)
    head_dim = getattr(config, "head_dim", config.hidden_size // config.num_attention_heads)
    kv_bytes_per_token = 4 * head_dim * config.num_key_value_heads * config.num_hidden_layers
    cache_pages = int(args.cache_gib * GIB / (16 * kv_bytes_per_token))
    max_context = max(args.lengths) + args.output_tokens
    if max_context > config.max_position_embeddings:
        raise SystemExit("Model context must cover the largest input plus generation")
    if args.gpu_gib * GIB < max_context * kv_bytes_per_token:
        raise SystemExit("GPU KV budget cannot hold even one longest request")
    args.out.mkdir(parents=True, exist_ok=args.resume)
    on_disk = any(mode.endswith("-disk") for mode in args.modes)
    if on_disk:
        args.disk_dir.mkdir(parents=True, exist_ok=True)
    manifest = {
        "model": model, "args": vars(args), "kv_bytes_per_token": kv_bytes_per_token,
        "vllm": importlib.metadata.version("vllm"), "torch": importlib.metadata.version("torch"),
        "kernel": os.uname().release,
        "batch_invariant": os.environ.get("VLLM_BATCH_INVARIANT", "0"),
        "gpu": subprocess.check_output(["nvidia-smi", "--query-gpu=name,memory.total,driver_version",
                                         "--format=csv,noheader"], text=True).strip(),
    }
    if on_disk:
        manifest["disk_mount"] = subprocess.check_output(
            ["findmnt", "-T", args.disk_dir, "-n", "-o", "SOURCE,FSTYPE"], text=True
        ).strip()
        manifest["block_devices"] = json.loads(subprocess.check_output(
            ["lsblk", "-J", "-o", "NAME,TYPE,TRAN,MODEL"], text=True
        ))
        source = manifest["disk_mount"].split()[0]
        transports = subprocess.run(["lsblk", "-sno", "TRAN", source], text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout.split()
        manifest["native_nvme"] = "microsoft" not in manifest["kernel"].lower() and "nvme" in transports
    environment = args.out / "environment.json"
    if environment.exists():
        previous = json.loads(environment.read_text())
        for key in ("model", "lengths", "shared", "requests", "output_tokens", "concurrency", "gpu_gib", "cache_gib", "seed"):
            if previous["args"].get(key) != vars(args).get(key):
                raise RuntimeError(f"Cannot resume a different workload: {key}")
        environment = args.out / f"environment-resume-{time.time_ns()}.json"
    environment.write_text(json.dumps(manifest, indent=2, default=str))
    if args.require_native_nvme and not manifest.get("native_nvme"):
        raise SystemExit("The disk path does not resolve to NVMe on native Linux")
    reference = {}
    if args.reference:
        for row in read_events(args.reference):
            reference[row["prompt_sha256"]] = row["output_ids"]
    for mode in args.modes:
        run = args.out / mode
        if args.resume and run.exists():
            done = read_events(run / "summary.jsonl")
            expected = {(length, math.floor(length * shared / 16) * 16)
                        for length in args.lengths for shared in args.shared}
            if ({(row["prompt_tokens"], row["shared_tokens_offered"]) for row in done} != expected
                    or any(row["requests"] != args.requests for row in done)):
                raise RuntimeError(f"Partial mode must be archived before resume: {run}")
            recorded = read_events(run / "requests.jsonl")
            if len(recorded) != len(expected) * args.requests:
                raise RuntimeError(f"Incomplete raw request records: {run}")
            if mode == "recompute":
                reference.update({row["prompt_sha256"]: row["output_ids"] for row in recorded})
            print(f"Completed mode retained: {run}", flush=True)
            continue
        run.mkdir()
        with ExitStack() as stack:
            options, lmcache_url, env = ["--no-enable-prefix-caching"], None, {}
            if mode == "native":
                options = ["--enable-prefix-caching", "--kv-offloading-backend", "native",
                           "--kv-offloading-size", str(args.cache_gib)]
            if mode.startswith("kvfabric"):
                addr = stack.enter_context(directory(args.directory, run / "directory.log", cache_pages + 64))
                options += kvfabric_config({
                    "library": str(args.library.resolve()), "control": addr, "cache_pages": cache_pages,
                    "load_timeout_ms": args.load_timeout_ms, "metrics_file": str((run / "kvfabric.jsonl").resolve()),
                    "cuda_batch": not args.legacy_cuda,
                    "disk_dir": str(args.disk_dir.resolve()) if mode.endswith("-disk") else None,
                })
            if mode.startswith("lmcache"):
                port, http_port = free_port(), free_port()
                command = [args.lmcache, "server", "--host", "127.0.0.1", "--port", str(port),
                           "--http-host", "127.0.0.1", "--http-port", str(http_port),
                           "--l1-size-gb", str(args.disk_cpu_gib if mode.endswith("-disk") else args.cache_gib),
                           "--eviction-policy", "LRU", "--chunk-size", str(args.lmcache_chunk)]
                if mode.endswith("-disk"):
                    folder = stack.enter_context(tempfile.TemporaryDirectory(prefix="kvf-lmcache-", dir=args.disk_dir))
                    command += ["--l2-adapter", json.dumps({
                        "type": "fs_native", "base_path": folder, "use_odirect": True,
                        "max_capacity_gb": args.cache_gib, "eviction": {"eviction_policy": "LRU"},
                    })]
                service_log = run / "lmcache.log"
                if args.lmcache_transfer_mode != "auto":
                    command += ["--supported-transfer-mode", args.lmcache_transfer_mode]
                    env["LMCACHE_MP_TRANSFER_MODE"] = args.lmcache_transfer_mode
                lm_env = dict(os.environ, LMCACHE_DISABLE_BANNER="1", LMCACHE_TRACK_USAGE="false")
                child = stack.enter_context(process(command, lm_env, service_log))
                lmcache_url = f"http://127.0.0.1:{http_port}"
                await_ready(child, service_log, lambda: get(f"{lmcache_url}/healthcheck") is not None)
                options += transfer_config("LMCacheMPConnector",
                                           {"lmcache.mp.host": "tcp://127.0.0.1", "lmcache.mp.port": port})
            url = stack.enter_context(server(model, run / "vllm.log", max_context,
                                             int(args.gpu_gib * GIB), args.concurrency, options, env))
            rng = random.Random(args.seed)
            generate(url, [100 + rng.randrange(20000) for _ in range(64)], 4)
            with (run / "requests.jsonl").open("w") as raw, (run / "summary.jsonl").open("w") as summaries:
                for length in args.lengths:
                    for shared in args.shared:
                        if mode.startswith("kvfabric"):
                            wait_kvf_idle(run / "kvfabric.jsonl")
                        prefix_count = math.floor(length * shared / 16) * 16
                        rng = random.Random(args.seed + length * 100000 + prefix_count)
                        prefix = [100 + rng.randrange(20000) for _ in range(prefix_count)]
                        if prefix:
                            disk_chunks = prefix_count // args.lmcache_chunk
                            if mode == "lmcache-disk" and disk_chunks:
                                reset_gpu_cache(url)
                                clear_lmcache_l1(lmcache_url)
                                before_disk = lmcache_disk_objects(lmcache_counters(lmcache_url))
                            primed = generate(url, prefix, 2)["request_id"]
                            if mode.startswith("kvfabric"):
                                wait_kvf_publication(run / "kvfabric.jsonl", primed, prefix_count)
                                wait_kvf_idle(run / "kvfabric.jsonl")
                            if mode == "lmcache-disk" and disk_chunks:
                                prime_lmcache_disk(lmcache_url, before_disk, disk_chunks)
                        # Use a separate deterministic RNG so measured prompts remain identical
                        # across independently launched modes and warmup-count changes.
                        warm_rng = random.Random(args.seed + length + prefix_count)
                        for _ in range(args.warmup_requests):
                            warm = generate(url, prefix + [100 + warm_rng.randrange(20000)
                                            for _ in range(length - prefix_count)], args.output_tokens)
                            if mode.startswith("kvfabric"):
                                cached = sum(e["tokens"] for e in read_events(run / "kvfabric.jsonl")
                                             if e["event"] == "hit"
                                             and e["request_id"].startswith(warm["request_id"] + "-"))
                                wait_kvf_publication(run / "kvfabric.jsonl", warm["request_id"], length - cached)
                                wait_kvf_idle(run / "kvfabric.jsonl")
                        reset_gpu_cache(url)
                        before = token_sources(get(f"{url}/metrics").decode())
                        lm_before = lmcache_counters(lmcache_url) if lmcache_url else {}
                        event_path = run / "kvfabric.jsonl"
                        event_count = len(read_events(event_path))
                        prompts = [prefix + [100 + rng.randrange(20000) for _ in range(length - prefix_count)]
                                   for _ in range(args.requests)]
                        with ThreadPoolExecutor(max_workers=args.concurrency) as pool:
                            if args.reset_gpu_between_batches:
                                # The native offloader requires GPU APC enabled. Clear only
                                # GPU APC between bursts to measure Host imports rather than
                                # the prefix left resident by an earlier measured request.
                                rows, elapsed = [], 0.0
                                for offset in range(0, len(prompts), args.concurrency):
                                    if offset:
                                        reset_gpu_cache(url)
                                    start = time.monotonic()
                                    rows.extend(pool.map(lambda ids: generate(url, ids, args.output_tokens),
                                                         prompts[offset:offset + args.concurrency]))
                                    elapsed += time.monotonic() - start
                            else:
                                start = time.monotonic()
                                rows = list(pool.map(lambda ids: generate(url, ids, args.output_tokens), prompts))
                                elapsed = time.monotonic() - start
                        after = token_sources(get(f"{url}/metrics").decode())
                        sources = {key: count - before.get(key, 0) for key, count in after.items()}
                        events = read_events(event_path)[event_count:]
                        kvf_hits = sum(event["tokens"] for event in events if event["event"] == "hit")
                        ext = sources.get("external_kv_transfer", 0)
                        hits = {
                            "gpu": sources.get("local_cache_hit"),
                            "cpu": ext if mode in ("native", "lmcache-cpu") else None,
                            "disk": None,
                            "external": sources.get("external_kv_transfer"),
                        }
                        if mode == "kvfabric-cpu":
                            hits["cpu"] = kvf_hits
                        if mode == "kvfabric-disk":
                            hits["disk"] = kvf_hits
                        lm_after = lmcache_counters(lmcache_url) if lmcache_url else {}
                        lm_delta = {name: n - lm_before.get(name, 0) for name, n in lm_after.items()}
                        if mode == "lmcache-disk" and "external_kv_transfer" in sources:
                            l1 = sum(n for name, n in lm_delta.items() if "_lookup_hit_l1_" in name
                                     and "_keys_" not in name)
                            l2 = sum(n for name, n in lm_delta.items() if "_lookup_hit_l2_" in name
                                     and "_keys_" not in name)
                            disk_chunks = sum(n for name, n in lm_delta.items()
                                              if "_l2_prefetch_load_completed_" in name)
                            # Attribute tokens only when independently counted GPU imports, lookups
                            # and completed disk chunks agree for the same sample window.
                            if l1 + l2 == ext and disk_chunks * args.lmcache_chunk == l2:
                                hits["cpu"], hits["disk"] = l1, l2
                        output = sum(len(row["output_ids"]) for row in rows)
                        checked = 0
                        for row in rows:
                            key = row["prompt_sha256"]
                            if mode == "recompute":
                                reference[key] = row["output_ids"]
                            elif reference:
                                if key not in reference:
                                    raise RuntimeError(f"Reference has no identical prompt: {key}")
                                if row["output_ids"] != reference[key]:
                                    raise RuntimeError(f"Output token mismatch: {mode} {key}")
                                checked += 1
                        result = {
                            "mode": mode, "prompt_tokens": length, "shared_tokens_offered": prefix_count,
                            "concurrency": args.concurrency, "warmup_requests": args.warmup_requests,
                            "output_tokens": args.output_tokens, "token_id_verified_requests": checked,
                            "gpu_reset_between_batches": args.reset_gpu_between_batches,
                            "throughput_scope": "sum of request bursts, excludes GPU reset RPCs"
                            if args.reset_gpu_between_batches else "continuous measured request window",
                            "requests": len(rows), "wall_s": elapsed, "req_s": len(rows) / elapsed,
                            "output_tokens_s": output / elapsed,
                            "total_tokens_s": (output + len(rows) * length) / elapsed,
                            "ttft_ms": percentile([r["ttft_ms"] for r in rows]),
                            "tpot_ms": percentile([r["tpot_ms"] for r in rows if r["tpot_ms"] is not None]),
                            "itl_ms": percentile([gap for r in rows for gap in r["itl_ms"]]),
                            "multi_token_chunks": sum(n > 1 for r in rows for n in r["output_chunk_sizes"]),
                            "token_sources": sources, "actual_hit_tokens": hits,
                            "lmcache_metrics_delta": lm_delta,
                            "actual_hit_rates": {tier: None if n is None else n / (len(rows) * length)
                                                 for tier, n in hits.items()},
                            "kvfabric_load_failures": sum(e["event"] == "load_failure" for e in events),
                            "kvfabric_save_skipped": sum(e["event"] == "save_skipped" for e in events),
                            "kvfabric_save_failures": sum(e["event"] == "save_failure" for e in events),
                            "kvfabric_io_mode": next((e["io_mode"] for e in read_events(event_path)
                                                     if e["event"] == "backend"), None),
                        }
                        if mode == "kvfabric-disk" and args.expected_io_mode:
                            if result["kvfabric_io_mode"] != args.expected_io_mode:
                                raise RuntimeError(f"Expected {args.expected_io_mode}, got {result['kvfabric_io_mode']}")
                        for row in rows:
                            raw.write(json.dumps({"shared_tokens_offered": prefix_count, **row}) + "\n")
                        raw.flush()
                        summaries.write(json.dumps(result) + "\n")
                        summaries.flush()
                        print(json.dumps(result), flush=True)
                        if mode.startswith("kvfabric"):
                            wait_kvf_idle(run / "kvfabric.jsonl")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model")
    parser.add_argument("--fallback-model")
    parser.add_argument("--lengths", nargs="+", type=int, default=[4096, 8192, 16384])
    parser.add_argument("--shared", nargs="+", type=float, default=[0, 0.5, 0.9])
    parser.add_argument("--modes", nargs="+", choices=MODES, default=MODES)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--disk-dir", type=Path)
    parser.add_argument("--require-native-nvme", action="store_true")
    parser.add_argument("--library", type=Path)
    parser.add_argument("--directory", type=Path)
    parser.add_argument("--lmcache", default=shutil.which("lmcache") or "lmcache")
    parser.add_argument("--lmcache-chunk", type=int, default=256)
    parser.add_argument("--lmcache-transfer-mode", choices=("auto", "lmcache_driven", "engine_driven"),
                        default="auto")
    parser.add_argument("--disk-cpu-gib", type=float, default=2.5)
    parser.add_argument("--cache-gib", type=float, default=4)
    parser.add_argument("--gpu-gib", type=float, default=3)
    parser.add_argument("--concurrency", type=int, default=1)
    parser.add_argument("--requests", type=int, default=12)
    parser.add_argument("--output-tokens", type=int, default=128)
    parser.add_argument("--load-timeout-ms", type=int, default=30000)
    parser.add_argument("--seed", type=int, default=123)
    parser.add_argument("--warmup-requests", type=int, default=3)
    parser.add_argument("--reference", type=Path, help="recompute requests.jsonl from an identical workload")
    parser.add_argument("--legacy-cuda", action="store_true", help="measure the original per-segment connector")
    parser.add_argument("--expected-io-mode", choices=("direct-uring-fixed", "direct-posix"))
    parser.add_argument("--reset-gpu-between-batches", action="store_true",
                        help="force Host/offload imports between measured bursts; excludes reset RPCs from throughput")
    parser.add_argument("--resume", action="store_true", help="retain complete modes from an identical workload")
    args = parser.parse_args()
    if any(m.startswith("kvfabric") for m in args.modes) and not (args.library and args.directory):
        parser.error("KV-Fabric modes require --library and --directory")
    if any(m.endswith("-disk") for m in args.modes) and not args.disk_dir:
        parser.error("Disk modes require --disk-dir")
    if min(args.requests, args.concurrency, args.output_tokens, args.cache_gib, args.gpu_gib,
           args.disk_cpu_gib, args.lmcache_chunk, *args.lengths) <= 0:
        parser.error("request, concurrency, generation and length values must be positive")
    if any(not 0 <= fraction < 1 for fraction in args.shared):
        parser.error("--shared fractions must be in [0, 1)")
    benchmark(args)


if __name__ == "__main__":
    main()
