#!/usr/bin/env python3
"""Whitelist the performance audit's public measurements and figures.

Never recurse over the raw archive: it contains local paths, profiler captures,
service logs, initial workspace state and private resume backups. Numeric
measurements are copied without rounding or re-estimation. Source captures
remain local; SHA256 provenance and the public-file manifest support review.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import shutil
import sys

from summarize_native_ncu import rows

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--archive", type=Path, default=ROOT / "docs/benchmarks/perf-attribution-2026-11-26")
    p.add_argument("--historical", type=Path, default=ROOT / "docs/benchmarks/native-linux-final")
    p.add_argument("--destination", type=Path, required=True, help="new, empty public evidence directory")
    a = p.parse_args()
    if a.destination.exists():
        p.error("destination must be new; refusing to mix with raw evidence")
    a.destination.mkdir(parents=True)
    provenance = []

    def copy(source, relative):
        dest = a.destination / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, dest)
        provenance.append({"public_path": relative, "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                           "transformation": "byte-for-byte"})

    def write(relative, value):
        dest = a.destination / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")

    def strip_paths(value):
        if isinstance(value, dict):
            return {k: strip_paths(v) for k, v in value.items()}
        if isinstance(value, list):
            return [strip_paths(v) for v in value]
        if isinstance(value, str) and value.startswith("/"):
            # Export source references as repository-relative names only.
            marker = "/docs/benchmarks/"
            if marker in value:
                return "docs/benchmarks/" + value.split(marker, 1)[1]
            if value.endswith(("/ncu-root/gather.csv", "/ncu-root/scatter.csv")):
                return "docs/benchmarks/native-linux-final/ncu-root/" + Path(value).name
            raise ValueError("Unapproved absolute path in selected measurement")
        return value

    for name in ("validation-summary.json", "excluded-runs.json", "final-source-sha256.json",
                 "io-summary.json", "rdma-audit.json", "attribution.json", "ncu-audit.json"):
        source = a.archive / name
        value = strip_paths(json.loads(source.read_text()))
        write(name, value)
        provenance.append({"public_path": name, "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                           "transformation": "JSON formatting; repository-relative source references"})
    for stage in ("matrix-before", "matrix-final"):
        for name in ("summary.jsonl", "requests.jsonl"):
            copy(a.archive / stage / "kvfabric-disk" / name, stage + "/kvfabric-disk/" + name)
    for source in sorted(a.archive.glob("io-r*-c*-*.json")):
        copy(source, source.name)
        copy(source.with_suffix(".log"), source.with_suffix(".csv").name)
    for name in ("rdma-payload-final", "rdma-payload-mtu9000", "rdma-payload-mtu1500-recheck"):
        for file in ("raw.csv", "summary.json"):
            copy(a.archive / name / file, name + "/" + file)
    copy(a.archive / "rdma-strict-final/summary.json", "rdma-strict-final/summary.json")
    copy(a.archive / "before/bridge-diagnostic.c", "before/bridge-diagnostic.c")
    copy(a.archive / "optimization.patch", "optimization.patch")
    for name in ("architecture", "nsys-read-concurrency", "ncu-bandwidth-accounting"):
        copy(a.archive / "figures" / (name + ".png"), "figures/" + name + ".png")
    for subdir, base in (("ncu-root", a.historical / "ncu-root"), ("ncu", a.archive / "ncu")):
        for kernel in ("gather", "scatter"):
            source = base / (kernel + ".csv")
            selected = [r for r in rows(source) if "__" in r["Metric Name"]]
            dest = a.destination / subdir / (kernel + ".csv")
            dest.parent.mkdir(parents=True, exist_ok=True)
            with dest.open("w", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=("Metric Name", "Metric Unit", "Metric Value"))
                writer.writeheader()
                for r in selected:
                    writer.writerow({k: r[k] for k in writer.fieldnames})
            provenance.append({"public_path": str(dest.relative_to(a.destination)),
                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "transformation": "metric names/units/values only; process/host identifiers removed"})
    # Audit parser validation contains original absolute source paths; regenerate its public copy.
    source = a.archive / "ncu-parser-validation.json"
    write("ncu-parser-validation.json", strip_paths(json.loads(source.read_text())))
    write("provenance.json", provenance)
    readme = """# Public performance attribution evidence — 2026-11-26

This is a whitelist export of real measurements. It excludes full nsys/ncu
captures, SQLite databases, host/process logs, environment paths, credentials,
private resume backups, models and build products. No measured number was
estimated or replaced. Full captures remain in the local audit archive.

- `matrix-before` / `matrix-final`: unprofiled 16K/8K-hit single-request
  Qwen2.5-3B BF16 results, exact output-token reference checks, and error counts.
- `attribution.json`: per-request phase intervals, nested timing, concurrency
  and exact 288 MiB H2D coverage assertions from the source SQLite captures.
  Thread sums and wall unions are different fields; never add them together.
- `ncu-root`: historical raw metric names/units/values, without identifiers;
  `ncu`: current repeated core counters. `ncu-audit.json` reconciles traffic,
  payload bandwidth and measured memory clocks.
- `io-r*.csv` / `.json`: equal-geometry file I/O, 200 requests per cell,
  process CPU sys/RSS and file-specific mincore residency. `io-summary.json`
  uses `.log` as original raw filenames; those files are exported here as CSV.
- `rdma-payload-final`: veth MTU 1500; `rdma-payload-mtu9000`: MTU 9000;
  `rdma-payload-mtu1500-recheck`: 16 MiB A/B/A recheck. Strict rsockets, actual
  observed transport, byte verification and connection/control/CRC/commit
  inclusive timing. RXE is software RoCE on one host, not a physical HCA SLA.
- `excluded-runs.json`: rejected early experiments and why they were excluded.
- `before/bridge-diagnostic.c`: original working-tree bridge with equivalent
  timing instrumentation, used by `scripts/build_perf_baseline.py`.
- `figures`: rendered from source counters and actual profiler intervals;
  the ncu panel is not a GUI screenshot.

`provenance.json` records SHA256 of source captures used for export. The public
manifest hashes the bytes that are actually published. To rerun experiments,
see `docs/PERFORMANCE-ATTRIBUTION-AUDIT.md`; full trace rendering requires newly
captured SQLite files, which are deliberately not in this public repository.
Existing PDF/DOCX resume exports predate this audit; the updated Markdown is
the authoritative version for this performance revision.
"""
    (a.destination / "README.md").write_text(readme)
    manifest = {str(f.relative_to(a.destination)): {"bytes": f.stat().st_size,
                "sha256": hashlib.sha256(f.read_bytes()).hexdigest()}
                for f in sorted(a.destination.rglob("*")) if f.is_file()}
    write("manifest-sha256.json", manifest)
    print(json.dumps({"files": len(manifest) + 1, "bytes": sum(x["bytes"] for x in manifest.values())}))


if __name__ == "__main__":
    main()
