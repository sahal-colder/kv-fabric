"""Real vLLM/BF16 round trips over a live KV-Fabric directory and Worker."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import random
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "integrations" / "vllm"))

from bench_vllm_kv import (
    directory, generate, kvfabric_config, model_path, read_events, server, wait_kvf_publication,
)
from kvfabric_vllm.bridge import Bridge, Spec


def check_io_mode(mode, uses_disk, require_uring_fixed):
    if uses_disk and not require_uring_fixed:
        assert mode.startswith("direct-"), f"Expected direct I/O, got {mode}"
    else:
        expected = "direct-uring-fixed" if uses_disk else "cpu"
        assert mode == expected, f"Expected {expected}, got {mode}"


def exercise_bridge(args, control, out):
    rng = random.Random(101)
    tokens = list(range(25, 29))
    for disk in (None, out / "bridge-cache"):
        zone = b"bridge-disk" if disk else b"bridge-cpu"
        spec = Spec(control.encode(), b"bf16-test", zone, 4, 3, 4096)
        source, sink = [Bridge(str(args.library), spec) for _ in range(2)]
        try:
            io_mode = source.open("bridge-source", os.fsencode(disk) if disk else None, 2, 2)
            check_io_mode(io_mode, disk is not None, args.require_uring_fixed)
            sink.open("bridge-sink", None, 2, 2)
            ids, hashes = source.chain(tokens)
            size = spec.layer_bytes * spec.layers
            data = rng.randbytes(size)
            original = ctypes.create_string_buffer(data, size)
            received = ctypes.create_string_buffer(size)
            assert source.lookup(hashes, 0, 1, 1000) == 0
            assert source.transfer(ids, hashes, 0, 1, ctypes.addressof(original)) == 0
            assert sink.lookup(hashes, 0, 1, 1000) == 1
            assert sink.transfer(ids, hashes, 0, 1, ctypes.addressof(received), 1000) == 0
            assert received.raw == data
            if not disk:
                ctypes.memset(received, 0, size)
                assert source.load_owned(hashes, 0, 1, 1000, ctypes.addressof(received)) == 1
                assert received.raw == data
            # Reuse the cache past its configured capacity with unrelated roots.
            for offset in (16, 32):
                new_ids, new_hashes = source.chain([token + offset for token in tokens])
                assert source.transfer(new_ids, new_hashes, 0, 1, ctypes.addressof(original)) == 0
                assert sink.lookup(new_hashes, 0, 1, 1000) == 1
            assert source.lookup(hashes, 0, 1, 1000) == 0
            print(json.dumps({"bridge": io_mode, "matched_bytes": size, "eviction": True}), flush=True)
        finally:
            sink.close()
            source.close()


def exercise_vllm(args, model, control, out, cases):
    rng = random.Random(202)
    layers = json.loads((Path(model) / "config.json").read_text())["num_hidden_layers"]
    build = lambda n: [100 + rng.randrange(20000) for _ in range(n)]
    whole, prefix, tail = build(512), build(256), build(320)
    targets = {"cold": whole, "hot": whole, "partial": prefix + tail, "timeout": whole}
    with server(model, out / "cold.log", 1536, 256 << 20, 2, ["--no-enable-prefix-caching"]) as url:
        reference = {label: generate(url, target, 8)["output_ids"]
                     for label, target in targets.items() if label in ("cold", "partial")}
    for variant in cases:
        metrics = out / f"{variant}.jsonl"
        config = {"library": str(args.library), "control": control, "tenant": variant,
                  "cache_pages": 160, "verify_gpu": True, "metrics_file": str(metrics)}
        if variant == "disk":
            config["disk_dir"] = str(out)
        if variant == "timeout":
            config["load_timeout_ms"] = 1
        with server(model, out / f"{variant}.log", 1536, 256 << 20, 2,
                    ["--no-enable-prefix-caching", *kvfabric_config(config)]) as url:
            mode = next(event["io_mode"] for event in read_events(metrics) if event["event"] == "backend")
            check_io_mode(mode, variant == "disk", args.require_uring_fixed)

            def run(label, expected, hit_tokens=0):
                before = len(read_events(metrics))
                result = generate(url, targets[label], 8)
                events = read_events(metrics)[before:]
                hits = [event for event in events if event["event"] == "hit"]
                failures = [event for event in events if event["event"] == "load_failure"]
                assert result["output_ids"] == expected, (label, result, expected)
                assert sum(event["tokens"] for event in hits) == hit_tokens, (label, events)
                checks = sum(event["blocks_checked"] for event in hits)
                assert checks == hit_tokens // 16 * layers, (label, events)
                if label == "timeout":
                    assert len(failures) == 1 and "time" in failures[0]["status"], events
                else:
                    assert not failures, events
                assert not any(event["event"] == "save_failure" for event in events), events
                print(json.dumps({"variant": variant, "case": label, "output_ids": result["output_ids"],
                                  "hit_tokens": hit_tokens, "checked_gpu_layer_blocks": checks}), flush=True)
                return result["request_id"]

            cold = run("cold", reference["cold"])
            wait_kvf_publication(metrics, cold, len(whole))
            if variant == "timeout":
                run("timeout", reference["cold"])
                continue
            run("hot", reference["cold"], len(whole) - 16)
            primed = generate(url, prefix, 2)["request_id"]
            wait_kvf_publication(metrics, primed, len(prefix))
            run("partial", reference["partial"], len(prefix))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model")
    parser.add_argument("--fallback-model")
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True, help="new output directory on Linux ext4")
    parser.add_argument("--bridge-only", action="store_true")
    parser.add_argument("--require-uring-fixed", action="store_true",
                        help="require direct-uring-fixed for disk round trips")
    parser.add_argument("--variants", nargs="+", choices=("cpu", "disk", "timeout"),
                        default=("cpu", "disk", "timeout"))
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.library = args.library.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    with directory(args.directory, args.out / "directory.log") as control:
        exercise_bridge(args, control, args.out)
        if not args.bridge_only:
            exercise_vllm(args, model_path(args), control, args.out, args.variants)


if __name__ == "__main__":
    main()
