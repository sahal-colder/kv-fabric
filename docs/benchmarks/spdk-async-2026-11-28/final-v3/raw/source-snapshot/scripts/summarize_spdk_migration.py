#!/usr/bin/env python3
"""Keep migration batch latency and inference TTFT separate from Raw backend I/O."""
import argparse
import csv
import json
from pathlib import Path
import statistics
from summarize_spdk_async import percentile


def migration(directory):
    groups, pairs = {}, {}
    for path in sorted(directory.glob('b*-pages*-qd*.csv')):
        batch = int(path.name.split('-')[0][1:])
        for row in csv.DictReader(path.open()):
            if int(row['sample']) < 0:
                continue
            key = (row['backend'], row['device'], int(row['page_bytes']), int(row['pages']),
                   int(row['concurrency']), row['direction'])
            groups.setdefault(key, []).append(row)
            match = (batch,) + key[1:] + (int(row['sample']),)
            pairs.setdefault(match, {})[key[0]] = int(row['elapsed_ns'])
    result = []
    for key, rows in sorted(groups.items()):
        backend, device, size, pages, qd, direction = key
        latencies = [int(row['elapsed_ns'])/1e6 for row in rows]
        throughput = [size*pages/int(row['elapsed_ns'])*1e9/(1 << 30) for row in rows]
        item = dict(layer='B-migration', backend=backend, device=device, page_bytes=size,
                    pages=pages, concurrency=qd, direction=direction, samples=len(rows),
                    gib_s_median=statistics.median(throughput), max_running=max(int(r['max_running']) for r in rows))
        for name, p in [('p50_batch_ms', .5), ('p95_batch_ms', .95), ('p99_batch_ms', .99), ('p999_batch_ms', .999)]:
            item[name] = percentile(latencies, p)
        for stage in ['io', 'crc', 'host_copy', 'd2h', 'h2d']:
            item[stage+'_cumulative_ms_median'] = statistics.median(int(r[stage+'_stage_ns']) for r in rows)/1e6
        result.append(item)
    paired = []
    for device, size, pages, qd, direction in sorted({k[1:] for k in groups}):
        values = [v['uring']/v['spdk'] for k, v in pairs.items()
                  if k[1:6] == (device, size, pages, qd, direction) and {'uring', 'spdk'} <= v.keys()]
        if values:
            paired.append(dict(device=device, page_bytes=size, pages=pages, concurrency=qd,
                               direction=direction, paired_rounds=len(values),
                               spdk_over_uring_throughput_ratio_median=statistics.median(values)))
    return {'groups': result, 'paired': paired, 'latency_scope': 'entire batch, not per-Page latency',
            'stage_scope': 'cumulative overlapping wall times; do not add to reconstruct elapsed time'}


def inference(directory):
    groups = {}
    for path in sorted(directory.glob('b*-inference.csv')):
        for row in csv.DictReader(path.open()):
            if int(row['sample']) < 0:
                continue
            groups.setdefault((row['backend'], row['cache_case']), []).append(row)
    result = []
    for (backend, case), rows in sorted(groups.items()):
        values = [int(r['ttft_ns'])/1e6 for r in rows]
        item = dict(layer='C-inference', backend=backend, cache_case=case, samples=len(rows),
                    token_ids_equal=all(r['token_ids_equal'] == 'True' for r in rows),
                    restore_wait_ms_median=statistics.median(int(r['ssd_restore_wait_ns']) for r in rows)/1e6)
        for name, p in [('p50_ttft_ms', .5), ('p95_ttft_ms', .95), ('p99_ttft_ms', .99), ('p999_ttft_ms', .999)]:
            item[name] = percentile(values, p)
        result.append(item)
    return {'groups': result, 'scope': 'generated dim=32, layers=2 CUDA fixture; no production model speedup claim',
            'wait_scope': 'synchronous inference scheduler wait for SSD restore, not GPU hardware idle counter'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('layer', choices=['migration', 'inference'])
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    result = migration(args.directory) if args.layer == 'migration' else inference(args.directory)
    (args.directory/'summary.json').write_text(json.dumps(result, indent=2)+'\n')
    rows = result['groups']
    if rows:
        with (args.directory/'summary.csv').open('w') as output:
            writer = csv.DictWriter(output, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    print(f'{len(rows)} {args.layer} groups')


if __name__ == '__main__':
    main()
