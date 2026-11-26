# Public performance attribution evidence — 2026-11-26

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
