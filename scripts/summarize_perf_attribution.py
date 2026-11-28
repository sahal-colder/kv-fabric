#!/usr/bin/env python3
"""Read raw traces and measurements; keep wall coverage separate from thread sums."""
import argparse
from bisect import bisect_right
from collections import defaultdict
import csv
import json
import math
from pathlib import Path
import re
import sqlite3
import statistics

from summarize_nsys import merge, intersections, difference, duration


def trace(path):
    db = sqlite3.connect(path / "trace.sqlite")
    groups = defaultdict(list)
    for lo, hi, tid, name in db.execute("""SELECT n.start,n.end,n.globalTid,coalesce(n.text,s.value)
            FROM NVTX_EVENTS n LEFT JOIN StringIds s ON n.textId=s.id WHERE n.end>n.start"""):
        groups[name].append((lo, hi, tid))
    copies = db.execute("SELECT start,end,bytes,copyKind FROM CUPTI_ACTIVITY_KIND_MEMCPY").fetchall()
    api = db.execute("SELECT a.start,a.end,a.globalTid,s.value FROM OSRT_API a JOIN StringIds s ON a.nameId=s.id").fetchall()
    db.close()
    publication = defaultdict(list)
    for lo, hi, tid in groups["kvfabric.publish"]:
        publication[tid].append((lo, hi))
    publication = {t: merge(v) for t, v in publication.items()}
    starts = {t: [a for a, b in v] for t, v in publication.items()}

    def is_publish(lo, hi, tid):
        if tid not in publication:
            return False
        i = bisect_right(starts[tid], lo) - 1
        return i >= 0 and publication[tid][i][1] >= hi

    requests = json.loads((path / "requests.json").read_text())["requests"]
    events = [json.loads(l) for l in (path / "kvfabric.jsonl").read_text().splitlines()]
    output = []
    for index, request in enumerate(requests):
        a, b, _ = groups[f"phase2.measured.{index}"][0]
        window = [(a, a + round(request["ttft_ms"] * 1e6))]

        def ranges(*names):
            return intersections(window, merge((a, b) for n in names for a, b, _ in groups[n]))

        fetch = ranges("kvfabric.fetch", "kvfabric.fetch.owned")
        disk = {}
        for name in ("disk_io.read", "disk_crc.read", "disk_stage.read", "disk_slot.acquire"):
            selected = [(a, b, t) for a, b, t in groups["kvfabric." + name]
                        if not is_publish(a, b, t) and any(x <= a and b <= y for x, y in fetch)]
            spans = [(a, b) for a, b, t in selected]
            disk[name] = {"count": len(spans), "thread_sum_ms": duration(spans) / 1e6,
                          "wall_union_ms": duration(merge(spans)) / 1e6}
            disk[name]["spans"] = merge(spans)
        assert disk["disk_io.read"]["count"] in (0, 512 * 36), disk["disk_io.read"]["count"]
        payload_copies = [(x, y, n) for x, y, n, kind in copies
                          if kind == 1 and n >= 1 << 20 and a <= x < window[0][1]]
        h2d_bytes = sum(n for _, _, n in payload_copies)
        assert h2d_bytes == 288 * 1024**2, h2d_bytes
        load = ranges("kvfabric.h2d", "kvfabric.scatter", "kvfabric.sync.load")
        tids = {t for lo, hi, t in groups["kvfabric.disk_io.read"]
                if not is_publish(lo, hi, t) and any(x <= lo and hi <= y for x, y in fetch)}
        tids |= {t for n in ("kvfabric.fetch", "kvfabric.fetch.owned")
                 for lo, hi, t in groups[n] if any(x <= lo and hi <= y for x, y in fetch)}
        locks = [(lo, hi) for lo, hi, t, n in api if t in tids and n == "pthread_mutex_lock"
                 and any(x <= lo and hi <= y for x, y in fetch) and not is_publish(lo, hi, t)]
        # Explicit priority makes an additive wall-time account without counting
        # work on concurrent lanes twice. This is coverage, not a critical-path proof.
        occupied = []
        exclusive = {}
        for name in ("disk_io.read", "disk_crc.read", "disk_stage.read", "disk_slot.acquire"):
            spans = disk[name].pop("spans")
            exclusive[name] = duration(difference(spans, occupied)) / 1e6
            occupied = merge(occupied + spans)
        bridge = intersections(fetch, ranges("kvfabric.fetch.bridge_stage"))
        exclusive["bridge_stage"] = duration(difference(bridge, occupied)) / 1e6
        occupied = merge(occupied + bridge)
        exclusive["remaining_fetch"] = (duration(fetch) - duration(occupied)) / 1e6
        windows = [e for e in events if e["event"] == "load_window"
                   and e["request_id"].startswith(request["request_id"] + "-")]
        io_events = []
        for lo, hi, t in groups["kvfabric.disk_io.read"]:
            if not is_publish(lo, hi, t) and any(x <= lo and hi <= y for x, y in fetch):
                io_events += [(lo, 1), (hi, -1)]
        inflight = peak = 0
        for _, delta in sorted(io_events):
            inflight += delta
            peak = max(peak, inflight)
        output.append({"index": index, "request_id": request["request_id"], "ttft_ms": request["ttft_ms"],
                       "fetch_wall_ms": duration(fetch) / 1e6, "disk": disk,
                       "fetch_exclusive_coverage_ms": exclusive,
                       "h2d_scatter_sync_wall_ms": duration(load) / 1e6,
                       "h2d_dma_ms": sum(y-x for x,y,_ in payload_copies)/1e6,
                       "h2d_payload_bytes": h2d_bytes, "disk_io_peak_concurrent_api": peak,
                       "mutex_calls_above_1us": len(locks),
                       "mutex_call_thread_sum_above_1us_ms": duration(locks)/1e6,
                       "other_ttft_ms": request["ttft_ms"]-(duration(fetch)+duration(load))/1e6,
                       "python_load_windows": windows})
    diagnostic = []
    for line in (path / "vllm.log").read_text().splitlines():
        if "KVF_FETCH " in line:
            diagnostic.append(json.loads(line.split("KVF_FETCH ",1)[1]))
    # Capture begins after warmup. The final 64 batches per measured request are
    # unambiguous because each request loads exactly 512 pages in eight-page batches.
    if diagnostic:
        diagnostic = diagnostic[-len(requests)*64:]
        assert len(diagnostic) == len(requests)*64
        for i, row in enumerate(output):
            row["remote_timing_thread_sum_ms"] = {k: sum(x[k] for x in diagnostic[i*64:(i+1)*64])/1e6
                for k in diagnostic[0] if k.endswith("_ns")}
    return output


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("archive", type=Path)
    a = ap.parse_args()
    traces = {}
    for name in ("nsys-before-disk", "nsys-before-diagnostic", "nsys-cpu", "nsys-final-disk"):
        path = a.archive / name
        if (path / "trace.sqlite").exists():
            traces[name] = trace(path)
    (a.archive / "attribution.json").write_text(json.dumps(traces, indent=2))
    io = []
    for p in sorted(a.archive.glob("io-r*-c*-*.json")):
        r = json.loads(p.read_text())
        rows = list(csv.DictReader(p.with_suffix(".log").open()))
        values = sorted(float(x["latency_ms"]) for x in rows)
        assert len(values) == r["requests"] and r["verified"]
        r.update(raw=p.with_suffix(".log").name, repeat=int(re.search(r"io-r(\d+)",p.name)[1]),
                 latency_ms={k:values[math.ceil(q*len(values))-1] for k,q in (("p50",.5),("p95",.95),("p99",.99))},
                 payload_GiB_s=sum(int(x["payload_bytes"]) for x in rows)/2**30/r["wall_s"])
        io.append(r)
    (a.archive / "io-summary.json").write_text(json.dumps(io, indent=2))
    print(json.dumps({n: [{"ttft_ms": x["ttft_ms"], "fetch_wall_ms": x["fetch_wall_ms"],
                         "peak_io": x["disk_io_peak_concurrent_api"]} for x in r]
                      for n,r in traces.items()}, indent=2))


if __name__ == "__main__":
    main()
