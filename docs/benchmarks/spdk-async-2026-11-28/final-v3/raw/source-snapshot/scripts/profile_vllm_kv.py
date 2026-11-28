"""Replay the Phase 1 16K inputs with the same 8K shared prefix and vLLM settings."""
import argparse
import json
import os
from pathlib import Path
import random
import urllib.request

import nvtx

from bench_vllm_kv import (
    HTTP, directory, generate, get, kvfabric_config, read_events, reset_gpu_cache, server,
    token_sources, wait_kvf_publication, wait_kvf_idle,
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("baseline", "optimized", "recompute"))
    parser.add_argument("build", type=Path)
    parser.add_argument("model")
    parser.add_argument("out", type=Path)
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--reference", type=Path, help="same workload from the recompute mode")
    parser.add_argument("--requests", type=int, default=2)
    parser.add_argument("--disk-dir", type=Path, help="profile the physical file-backed NVMe tier")
    parser.add_argument("--io-depth", type=int, default=4)
    parser.add_argument("--warmup-requests", type=int, default=5,
                        help="full-length warmups; five fill the bounded 1.5GiB external cache")
    args = parser.parse_args()
    model_config = json.loads((Path(args.model) / "config.json").read_text())
    head_dim = model_config.get("head_dim", model_config["hidden_size"] // model_config["num_attention_heads"])
    kv_bytes_per_token = 4 * head_dim * model_config["num_key_value_heads"] * model_config["num_hidden_layers"]
    rng = random.Random(123)

    def tokens(n):
        return [100 + rng.randrange(20000) for _ in range(n)]

    warmup = tokens(64)
    # Match bench_vllm_kv.py independently of other lengths in its sweep.
    rng = random.Random(123 + 16384 * 100000 + 8192)
    prefix = tokens(8192)
    prompts = [prefix + tokens(8192) for _ in range(args.requests)]
    args.out.mkdir(parents=True, exist_ok=True)
    events_path = (args.out / "kvfabric.jsonl").resolve()
    if events_path.exists():
        raise SystemExit("Use a fresh output directory")
    with directory(args.build / "bin/kvfabricd", args.out / "directory.log", 3000) as ctl:
        config = dict(library=str((args.build / "src/integration/libkvf_vllm.so").resolve()),
                      control=ctl, cache_pages=2730, load_timeout_ms=30000,
                      metrics_file=str(events_path), cuda_batch=args.mode == "optimized")
        if args.disk_dir:
            config.update(disk_dir=str(args.disk_dir.resolve()), io_depth=args.io_depth)
        options = ["--no-enable-prefix-caching"]
        if args.mode != "recompute":
            options += kvfabric_config(config)
        if args.profile:
            options += ["--profiler-config", '{"profiler":"cuda"}']
        with server(args.model, args.out / "vllm.log", 16400, 1 << 30, 1, options) as url:
            generate(url, warmup, 4)
            with nvtx.annotate("phase2.prime"):
                primed = generate(url, prefix, 2)["request_id"]
                if args.mode != "recompute":
                    wait_kvf_publication(events_path, primed, len(prefix))
                    wait_kvf_idle(events_path)
            warm_rng = random.Random(123 + 16384 + 8192)
            for i in range(args.warmup_requests):
                with nvtx.annotate(f"phase2.warmup.{i}"):
                    warm = generate(url, prefix + [100 + warm_rng.randrange(20000)
                                                  for _ in range(8192)], 16)
                    if args.mode != "recompute":
                        cached = sum(e["tokens"] for e in read_events(events_path)
                                     if e["event"] == "hit"
                                     and e["request_id"].startswith(warm["request_id"] + "-"))
                        wait_kvf_publication(events_path, warm["request_id"], 16384 - cached)
                        wait_kvf_idle(events_path)
            reset_gpu_cache(url)
            before = token_sources(get(url + "/metrics").decode())
            if args.profile:
                with HTTP.open(urllib.request.Request(url + "/start_profile", method="POST"), timeout=30):
                    pass
            rows = []
            for i, prompt in enumerate(prompts):
                with nvtx.annotate(f"phase2.measured.{i}"):
                    rows.append(generate(url, prompt, 16))
            if args.profile:
                with HTTP.open(urllib.request.Request(url + "/stop_profile", method="POST"), timeout=30):
                    pass
            after = token_sources(get(url + "/metrics").decode())
            if args.mode != "recompute":
                wait_kvf_idle(events_path)
            hits = {key: value - before.get(key, 0) for key, value in after.items()}
            # Repeating a complete target after its publication would turn a 50% hit into a near-full hit.
            assert hits.get("external_kv_transfer") == (0 if args.mode == "recompute" else len(rows) * 8192), hits
            if args.reference:
                reference = json.loads(args.reference.read_text())["requests"]
                assert [r["output_ids"] for r in rows] == [r["output_ids"] for r in reference]
            (args.out / "requests.json").write_text(json.dumps({
                "requests": rows, "token_sources": hits,
                "workload": {"input_tokens": 16384, "shared_tokens": 8192, "output_tokens": 16,
                             "shared_kv_bytes": 8192 * kv_bytes_per_token,
                             "concurrency": 1, "full_length_warmups": args.warmup_requests,
                             "external_cache_pages": 2730, "gpu_kv_bytes": 1 << 30,
                             "batch_invariant": os.environ.get("VLLM_BATCH_INVARIANT", "0"),
                             "queued_publications_drained_after_capture": args.mode != "recompute"},
            }, indent=2) + "\n")


if __name__ == "__main__":
    main()
