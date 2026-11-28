#!/usr/bin/env python3
"""Summarize only this run's matrix, never historical/WSL benchmark trees."""
import csv
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "docs/benchmarks/native-linux-final"


def main():
    global OUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, default=OUT)
    OUT = parser.parse_args().archive.resolve()
    run = json.loads((OUT / "run.json").read_text())
    results = []
    for matrix in sorted(OUT.glob("matrix-*")):
        if not matrix.is_dir():
            continue
        for summary in sorted(matrix.glob("*/summary.jsonl")):
            for line in summary.read_text().splitlines():
                row = json.loads(line)
                row["matrix"] = matrix.name
                row["source"] = str(summary.relative_to(OUT))
                results.append(row)
    (OUT / "matrix-summary.json").write_text(json.dumps({
        "run_id": run["run_id"], "cells": results,
        "limitations": ["P95/P99 with n=15 are both the observed maximum (nearest rank).",
            "Tokens/s means generated output tokens/s; total logical tokens/s is separate.",
            "ITL uses streamed chunk arrival times; inspect multi_token_chunks before interpreting as per-token.",
            "Fixed seeded synthetic token IDs, BF16, eager mode, single GPU; no production-traffic claim."]
    }, indent=2))
    fields = ["matrix", "mode", "context", "concurrency", "requests", "ttft_p50_ms", "ttft_p95_ms",
              "ttft_p99_ms", "tpot_p50_ms", "itl_p50_ms", "req_s", "output_tokens_s",
              "gpu_hit_rate", "cpu_hit_rate", "disk_hit_rate", "external_hit_rate",
              "token_id_verified_requests", "io_mode", "source"]
    fields[fields.index("io_mode"):fields.index("io_mode")] = [
        "load_failures", "save_failures", "save_skipped_events", "gpu_reset_between_batches", "throughput_scope"]
    with (OUT / "matrix-summary.csv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for row in results:
            writer.writerow({"matrix": row["matrix"], "mode": row["mode"], "context": row["prompt_tokens"],
                "concurrency": row["concurrency"], "requests": row["requests"],
                **{f"ttft_{p}_ms": row["ttft_ms"][p] for p in ("p50", "p95", "p99")},
                "tpot_p50_ms": row["tpot_ms"]["p50"], "itl_p50_ms": row["itl_ms"]["p50"],
                "req_s": row["req_s"], "output_tokens_s": row["output_tokens_s"],
                **{f"{tier}_hit_rate": row["actual_hit_rates"][tier] for tier in ("gpu", "cpu", "disk", "external")},
                "token_id_verified_requests": row["token_id_verified_requests"],
                "load_failures": row.get("kvfabric_load_failures"),
                "save_failures": row.get("kvfabric_save_failures"),
                "save_skipped_events": row.get("kvfabric_save_skipped"),
                "gpu_reset_between_batches": row.get("gpu_reset_between_batches", False),
                "throughput_scope": row.get("throughput_scope", "continuous measured request window"),
                "io_mode": row["kvfabric_io_mode"], "source": row["source"]})
    print(f"Archived {len(results)} measured cells; no missing cells imputed")


if __name__ == "__main__":
    main()
