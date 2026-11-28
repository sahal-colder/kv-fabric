#!/usr/bin/env python3
"""All measured samples, stratified by layer/workload/size/depth; no best-round selection."""
import argparse
import csv
import json
from pathlib import Path
import statistics


def percentile(values, p):
    values = sorted(values)
    x = (len(values)-1)*p
    low = int(x)
    return values[low]+(values[min(low+1, len(values)-1)]-values[low])*(x-low)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    groups, paired = {}, {}
    for path in sorted(args.directory.glob('b*-rounds.csv')):
        batch = int(path.name.split('-')[0][1:])
        rows = [r for r in csv.DictReader(path.open()) if int(r['sample']) >= 0]
        for row in rows:
            key = (row['backend'], row['workload'], int(row['bytes']), int(row['qd']))
            groups.setdefault(key, []).append(row)
            match = (batch, row['workload'], int(row['bytes']), int(row['qd']), int(row['sample']))
            paired.setdefault(match, {})[row['backend']] = float(row['iops'])
    output = []
    for key, rows in sorted(groups.items()):
        backend, work, size, qd = key
        latencies = []
        for path in sorted(args.directory.glob(f'b*-{backend}-{size}-qd{qd}-{work}-io.csv')):
            for row in csv.DictReader(path.open()):
                if int(row['sample']) >= 0:
                    latencies.append((int(row['completed_ns'])-int(row['queued_ns']))/1000)
        ops = sum(int(r['operations']) for r in rows)
        seconds = sum(float(r['seconds']) for r in rows)
        item = {'layer': 'A-raw-backend', 'backend': backend, 'workload': work, 'bytes': size, 'qd': qd,
                'samples': len(rows), 'iops_median': statistics.median(float(r['iops']) for r in rows),
                'mib_s_median': statistics.median(float(r['mib_s']) for r in rows),
                'cpu_utilization_percent': sum(float(r['cpu_seconds']) for r in rows)/seconds*100,
                'cpu_us_per_io': sum(float(r['cpu_seconds']) for r in rows)/ops*1e6,
                'queue_wait_us_per_io': sum(int(r['queue_wait_ns']) for r in rows)/ops/1000,
                'copy_us_per_io': sum(int(r['copy_ns']) for r in rows)/ops/1000,
                'actual_max_inflight': max(int(r['max_inflight']) for r in rows),
                'verified_read_bytes': sum(int(r['verified_bytes']) for r in rows),
                'cpu_cycles_per_io': sum(float(r.get('cpu_cycles', 0)) for r in rows)/ops if all(int(r.get('cycles_measured', 0)) for r in rows) else None, 'iopoll': bool(int(rows[0]['iopoll'])),
                'sqpoll': bool(int(rows[0]['sqpoll']))}
        for name, p in [('p50_us', .5), ('p95_us', .95), ('p99_us', .99), ('p999_us', .999)]:
            item[name] = percentile(latencies, p)
        output.append(item)
    comparisons = []
    for work, size, qd in sorted({(k[1], k[2], k[3]) for k in groups}):
        matches = [(k, v) for k, v in paired.items() if k[1:4] == (work, size, qd) and {'uring', 'spdk'} <= v.keys()]
        ratios = [v['spdk']/v['uring'] for _, v in matches]
        if not ratios:
            continue
        batch_ratios = []
        for batch in sorted({k[0] for k, _ in matches}):
            values = [v for k, v in matches if k[0] == batch]
            batch_ratios.append(statistics.median(v['spdk'] for v in values)/statistics.median(v['uring'] for v in values))
        comparisons.append({'workload': work, 'bytes': size, 'qd': qd, 'paired_rounds': len(ratios),
                            'paired_process_batches': len(batch_ratios),
                            'spdk_over_uring_iops_ratio_median': statistics.median(ratios),
                            'process_batch_median_ratios': batch_ratios,
                            'inference': 'descriptive paired samples; within-process rounds are correlated; no significance claim'})
    result = {'groups': output, 'paired': comparisons, 'latency_scope': 'enqueue to actual completion callback, includes backend-owned memory copies',
              'cpu_scope': 'whole-process CPU time within timed I/O windows; CRC verification separate',
              'cycles_scope': 'PMU cycles across all process threads existing before measurement, within I/O windows plus counter-control boundary; excludes CRC verification'}
    (args.directory/'summary.json').write_text(json.dumps(result, indent=2)+'\n')
    if output:
        with (args.directory/'summary.csv').open('w') as target:
            writer = csv.DictWriter(target, fieldnames=list(output[0]))
            writer.writeheader()
            writer.writerows(output)
    print(f'{len(output)} backend groups, {len(comparisons)} paired cells')


if __name__ == '__main__':
    main()
