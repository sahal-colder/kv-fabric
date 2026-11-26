#!/usr/bin/env python3
"""Attribute TTFT and KV copies in an Nsight Systems SQLite export."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import sqlite3


def merge(spans):
    result = []
    for lo, hi in sorted((lo, hi) for lo, hi in spans if hi > lo):
        if result and lo <= result[-1][1]:
            result[-1] = (result[-1][0], max(result[-1][1], hi))
        else:
            result.append((lo, hi))
    return result


def intersections(left, right):
    result = []
    i = j = 0
    while i < len(left) and j < len(right):
        a, b = left[i]
        c, d = right[j]
        if min(b, d) > max(a, c):
            result.append((max(a, c), min(b, d)))
        if b < d:
            i += 1
        else:
            j += 1
    return result


def difference(spans, excluded):
    result = []
    cursor = 0
    for lo, hi in spans:
        while cursor < len(excluded) and excluded[cursor][1] <= lo:
            cursor += 1
        for left, right in excluded[cursor:]:
            if left >= hi:
                break
            if left > lo:
                result.append((lo, left))
            lo = max(lo, right)
            if lo >= hi:
                break
        if lo < hi:
            result.append((lo, hi))
    return result


def duration(spans):
    return sum(hi - lo for lo, hi in spans)


def read_trace(database, requests):
    with sqlite3.connect(f"{database.resolve().as_uri()}?mode=ro", uri=True) as db:
        tables = {row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        if not {"NVTX_EVENTS", "CUPTI_ACTIVITY_KIND_MEMCPY", "CUPTI_ACTIVITY_KIND_KERNEL"} <= tables:
            raise SystemExit("Trace lacks NVTX, CUDA GPU memcpy or CUDA kernels")
        groups = defaultdict(list)
        for lo, hi, tid, name in db.execute("""
            SELECT n.start, n.end, n.globalTid, coalesce(n.text, s.value)
            FROM NVTX_EVENTS n LEFT JOIN StringIds s ON s.id=n.textId
            WHERE n.end > n.start
        """):
            groups[name].append((lo, hi, tid))
        api = []
        for table in ("CUPTI_ACTIVITY_KIND_RUNTIME", "CUPTI_ACTIVITY_KIND_DRIVER"):
            if table in tables:
                api.extend(db.execute(f"SELECT start,end,globalTid,correlationId FROM {table}"))
        gpu_copy = db.execute("""
            SELECT start,end,bytes,copyKind,correlationId FROM CUPTI_ACTIVITY_KIND_MEMCPY
            WHERE copyKind IN (1,2)
        """).fetchall()
        kernels = db.execute("""
            SELECT k.start,k.end,s.value,k.correlationId
            FROM CUPTI_ACTIVITY_KIND_KERNEL k LEFT JOIN StringIds s ON s.id=k.demangledName
        """).fetchall()
    result = []
    for index, request in enumerate(requests):
        ranges = groups[f"phase2.measured.{index}"]
        if len(ranges) != 1:
            raise SystemExit(f"Missing unique request marker phase2.measured.{index}")
        start, end, _ = ranges[0]
        result.append(summarize_request(
            [(start, min(end, start + round(1e6 * request["ttft_ms"])))],
            groups, api, gpu_copy, kernels,
        ))
    return result


def summarize_request(window, groups, api, gpu_copy, kernels):
    def named(*names):
        return intersections(window, merge((a, b) for name in names for a, b, _ in groups[name]))

    def api_in_ranges(name):
        by_tid = defaultdict(list)
        for a, b, tid in groups[name]:
            if a < window[0][1] and b > window[0][0]:
                by_tid[tid].append((a, b))
        selected = defaultdict(list)
        ids = set()
        for a, b, tid, corr in api:
            if tid in by_tid and any(a < hi and b > lo for lo, hi in by_tid[tid]):
                selected[tid].append((a, b))
                ids.add(corr)
        spans = merge(span for tid in selected for span in intersections(
            merge(by_tid[tid]), merge(selected[tid])
        ))
        return duration(intersections(window, spans)), ids

    lookup = named("kvfabric.lookup")
    fetch = named("kvfabric.fetch")
    background_publish = named("kvfabric.publish.background")
    publish = difference(named("kvfabric.publish"), background_publish)
    owned = named("kvfabric.fetch.owned")
    acquire = named("kvfabric.detail.acquire", "kvfabric.detail.lease_finish", "kvfabric.detail.lease_validate")
    crc = named("kvfabric.detail.crc")
    bridge_copy = named("kvfabric.detail.bridge_copy")
    write_rpc = named("kvfabric.detail.heartbeat", "kvfabric.detail.publish_rpc")
    detail = any(name.startswith("kvfabric.detail.") for name in groups if name)
    h2d = named("kvfabric.h2d")
    d2h = named("kvfabric.d2h")
    sync_load = named("kvfabric.sync.load")
    sync_save = named("kvfabric.sync.save")
    h2d_api, h2d_ids = api_in_ranges("kvfabric.h2d")
    d2h_api, d2h_ids = api_in_ranges("kvfabric.d2h")
    kv_kernel_ids = h2d_ids | d2h_ids
    for name in ("kvfabric.gather", "kvfabric.scatter"):
        kv_kernel_ids |= api_in_ranges(name)[1]
    wall = duration(window)
    # PyTorch's legacy strided copies can launch packing kernels with generic
    # names. Exclude every kernel launched inside KV transfer ranges by CUDA
    # correlation ID, instead of counting those as model compute overlap.
    model = merge((row[0], row[1]) for row in kernels
                  if (len(row) < 4 or row[3] not in kv_kernel_ids)
                  and not any(key in (row[2] or "") for key in ("gather_kv_pages", "scatter_kv_pages")))
    output = {
        "ttft_ms": wall / 1e6,
        "phase_ms": {
            "scheduler_directory_lookup": duration(lookup) / 1e6,
            "bridge_fetch_wall": duration(fetch) / 1e6,
            "owned_fetch_wall": duration(owned) / 1e6,
            "fetch_directory_nested": duration(intersections(fetch, acquire)) / 1e6 if detail else None,
            "fetch_crc_nested": duration(intersections(fetch, crc)) / 1e6 if detail else None,
            "fetch_bridge_copy_nested": duration(intersections(fetch, bridge_copy)) / 1e6 if detail else None,
            "fetch_other": (duration(fetch) - duration(intersections(fetch, merge(acquire + crc + bridge_copy)))) / 1e6
            if detail else None,
            "h2d_submit": duration(h2d) / 1e6,
            "h2d_cuda_api_nested": h2d_api / 1e6,
            "h2d_host_dispatch_estimate": (duration(h2d) - h2d_api) / 1e6,
            "h2d_host_sync": duration(sync_load) / 1e6,
            "save_d2h_submit": duration(d2h) / 1e6,
            "save_d2h_cuda_api_nested": d2h_api / 1e6,
            "save_d2h_host_dispatch_estimate": (duration(d2h) - d2h_api) / 1e6,
            "save_host_sync": duration(sync_save) / 1e6,
            "save_publish_wall": duration(publish) / 1e6,
            "save_background_publish_wall": duration(background_publish) / 1e6,
            "save_sync_model_kernel_overlap": duration(intersections(sync_save, model)) / 1e6,
            "save_sync_other": duration(difference(sync_save, model)) / 1e6,
            "save_wait_publish_excluding_model_compute": duration(
                difference(merge(sync_save + publish), model)) / 1e6,
            "save_foreground_excluding_model_compute": duration(
                difference(merge(d2h + sync_save + publish), model)) / 1e6,
            "save_publish_rpc_nested": duration(intersections(publish, write_rpc)) / 1e6 if detail else None,
            "save_publish_crc_nested": duration(intersections(publish, crc)) / 1e6 if detail else None,
            "save_publish_bridge_copy_nested": duration(intersections(publish, bridge_copy)) / 1e6 if detail else None,
            "model_gpu_kernel_active": duration(intersections(window, model)) / 1e6,
        },
        "cuda_kv": {},
    }
    for key, kind, ids in (("h2d", 1, h2d_ids), ("d2h", 2, d2h_ids)):
        copies = [(a, b, n) for a, b, n, k, corr in gpu_copy
                  if k == kind and corr in ids and a < window[0][1] and b > window[0][0]]
        full_active = merge((a, b) for a, b, _ in copies)
        active = intersections(window, full_active)
        ns = duration(full_active)
        window_ns = duration(active)
        size = sum(n for _, _, n in copies)
        output["cuda_kv"][key] = {
            "count": len(copies), "bytes": size, "active_ms": ns / 1e6,
            "active_within_ttft_ms": window_ns / 1e6,
            "active_gib_s": (size / 2**30) / (ns / 1e9) if ns else None,
            "model_compute_overlap_ratio": duration(intersections(active, model)) / window_ns if window_ns else None,
            "bandwidth_scope": "complete DMA activities correlated to TTFT; includes tails crossing window end",
        }
    if not background_publish:
        # Original path's sequential Host ranges are the TTFT attribution.
        # Split out model kernels inside the wait. The residual includes first
        # sampling, HTTP, unmarked scheduler time, and work between ranges.
        used = merge(lookup + fetch + owned + h2d + sync_load + d2h + sync_save + publish)
        output["phase_ms"]["unmarked_ttft"] = duration(difference(window, used)) / 1e6
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sqlite", type=Path)
    parser.add_argument("requests", type=Path, help="JSON emitted by the traced request driver")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    workload = json.loads(args.requests.read_text())
    requests = workload["requests"]
    rows = read_trace(args.sqlite, requests)
    expected = workload.get("workload", {}).get("shared_kv_bytes")
    actual = [row["cuda_kv"]["h2d"]["bytes"] for row in rows]
    coverage = {"expected_h2d_bytes_per_request": expected, "actual_h2d_bytes": actual,
                "passed": all(n == expected for n in actual) if expected is not None else None}
    data = json.dumps({"profiled_requests": rows, "coverage_validation": coverage}, indent=2) + "\n"
    if args.out:
        args.out.write_text(data)
    else:
        print(data, end="")
    if coverage["passed"] is False:
        raise SystemExit(f"Incomplete CUDA activity capture; expected {expected} H2D bytes, got {actual}")


if __name__ == "__main__":
    main()
