#!/usr/bin/env python3
"""Extract explicitly measured Nsight Compute metrics, preserving units."""
import csv
import argparse
import io
import json
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / "docs/benchmarks/native-linux-final"


def rows(path):
    text = path.read_text()
    lines = text.splitlines()
    start = next((i for i, line in enumerate(lines) if '"Metric Name"' in line), None)
    if start is not None:
        return list(csv.DictReader(io.StringIO("\n".join(lines[start:]))))
    table = list(csv.reader(io.StringIO(text)))
    if len(table) != 3:
        raise ValueError("Expected one kernel's wide raw CSV, including its unit row")
    return [{"Metric Name": name, "Metric Unit": table[1][i], "Metric Value": table[2][i]}
            for i, name in enumerate(table[0])]


def main():
    global OUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, default=OUT)
    OUT = parser.parse_args().archive.resolve()
    results = []
    for name in ("gather", "scatter"):
        path = OUT / "ncu-root" / f"{name}.csv"
        metrics = {r["Metric Name"]: r for r in rows(path)}

        def measured(metric):
            r = metrics[metric]
            return {"metric": metric, "value": float(r["Metric Value"].replace(",", "")),
                    "unit": r["Metric Unit"]}

        dram_metric = next(key for key in ("gpu__dram_throughput.avg.pct_of_peak_sustained_elapsed",
                          "dram__throughput.avg.pct_of_peak_sustained_elapsed") if key in metrics)
        dram = measured(dram_metric)
        bandwidth = measured("dram__bytes.sum.per_second")
        factors = {"byte/second": 1, "byte/s": 1, "Kbyte/second": 1e3, "Mbyte/second": 1e6,
                   "Gbyte/second": 1e9, "Kbyte/s": 1e3, "Mbyte/s": 1e6, "Gbyte/s": 1e9}
        if bandwidth["unit"] not in factors:
            raise ValueError(f"Unknown measured bandwidth unit: {bandwidth}")
        occupancy = measured("sm__warps_active.avg.pct_of_peak_sustained_active")
        sector = {}
        for op in ("ld", "st"):
            metric = next((key for key in (
                f"smsp__sass_average_data_bytes_per_sector_mem_global_op_{op}.ratio",
                f"l1tex__average_t_bytes_per_sector_mem_global_op_{op}.ratio") if key in metrics), None)
            if metric in metrics:
                value = measured(metric)
                sector[op] = {**value, "sector_utilization_percent": value["value"] / 32 * 100}
        results.append({"kernel": name + "_kv_pages", "source": str(path), "dram_percent": dram,
                        "bandwidth": bandwidth,
                        "bandwidth_GiB_s": bandwidth["value"] * factors[bandwidth["unit"]] / 2**30,
                        "achieved_occupancy": occupancy, "global_sectors": sector,
                        "dram_clock": measured("dram__cycles_elapsed.avg.per_second"),
                        "kernel_duration": measured("gpu__time_duration.sum"),
                        "profiled_launches": 1, "replay_passes": 37,
                        "scope": "72 MiB synthetic page packing microbenchmark; not LLM TTFT"})
    (OUT / "ncu-summary.json").write_text(json.dumps(results, indent=2))
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
