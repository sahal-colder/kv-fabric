#!/usr/bin/env python3
"""CUDA memcpy activity overlapping other Pages' outstanding production SSD calls.

NVTX SSD ranges include dispatch/wait/copy, not pure NVMe device service time.
No instrumented timing is substituted for uninstrumented performance results.
"""
import argparse
import csv
import json
from pathlib import Path
import sqlite3


def overlap_duration(start, end, ranges):
    pieces = sorted((max(start, a), min(end, b)) for a, b, _ in ranges if a < end and b > start)
    total, last = 0, start
    for a, b in pieces:
        if b > last:
            total += b-max(a, last)
            last = b
    return total


def extract(path):
    con = sqlite3.connect(path)
    io = list(con.execute("SELECT start,end,globalTid,text FROM NVTX_EVENTS WHERE text LIKE 'kvfabric.profile.ssd.%' AND end IS NOT NULL ORDER BY start"))
    worker_ids = {row[2] for row in io}
    copies = list(con.execute('''SELECT m.start,m.end,m.copyKind,m.streamId,r.globalTid,m.bytes
        FROM CUPTI_ACTIVITY_KIND_MEMCPY m JOIN CUPTI_ACTIVITY_KIND_RUNTIME r
        ON m.correlationId=r.correlationId AND m.globalPid=(r.globalTid & -16777216)
        WHERE m.copyKind IN (1,2) ORDER BY m.start'''))
    # Nsight globalPid encoding is build-specific; correlation IDs in this
    # single-process trace are unique, and globalTid filters the worker owners.
    if not copies:
        copies = list(con.execute('''SELECT m.start,m.end,m.copyKind,m.streamId,r.globalTid,m.bytes
            FROM CUPTI_ACTIVITY_KIND_MEMCPY m JOIN CUPTI_ACTIVITY_KIND_RUNTIME r
            ON m.correlationId=r.correlationId WHERE m.copyKind IN (1,2) ORDER BY m.start'''))
    copies = [row for row in copies if row[4] in worker_ids]
    result = []
    for kind, phase, label in [(2, 'write_wait', 'D2H while another Page awaits SSD write'),
                                (1, 'read_wait', 'H2D while another Page awaits SSD read')]:
        selected = [row for row in copies if row[2] == kind]
        wait = [(a, b, tid) for a, b, tid, text in io if text.endswith(phase)]
        total = sum(b-a for a, b, *_ in selected)
        overlapped, count = 0, 0
        for a, b, _, _, tid, _ in selected:
            duration = overlap_duration(a, b, [(x, y, owner) for x, y, owner in wait if owner != tid])
            overlapped += duration
            count += duration > 0
        result.append({'scope': label, 'cuda_copy_count': len(selected), 'copy_bytes': sum(row[5] for row in selected),
                       'distinct_cuda_streams': len({row[3] for row in selected}), 'copies_with_cross_page_overlap': count,
                       'cuda_device_copy_total_ms': total/1e6, 'overlap_with_other_page_ssd_call_ms': overlapped/1e6,
                       'fraction_cuda_copy_time_covered_by_other_page_ssd_call': overlapped/total if total else None})
    con.close()
    return result, io, copies


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    run = json.loads((args.directory/'run.json').read_text())
    if run.get('error') or run.get('results') != {'uring': 0, 'spdk': 0} or not all(c['matches_original'] for c in run['checks']['after']):
        raise RuntimeError('trace run incomplete or original data not restored')
    result = {'scope': '128 x 512 KiB Pages, concurrency16, 3 warmups + 3 instrumented rounds',
              'interpretation': 'CUDA device memcpy intervals overlap other Pages\' outstanding SSD API calls. SSD calls include queues, CPU copies and completion wait; not a measurement of pure NVMe DMA service overlap.',
              'results': {}}
    traces = {}
    for backend in ['uring', 'spdk']:
        rows, io, copies = extract(args.directory/(backend+'.sqlite'))
        if any(row['cuda_copy_count'] != 6*128 or not row['copies_with_cross_page_overlap'] for row in rows):
            raise RuntimeError('missing expected memcpy or overlap evidence for '+backend)
        result['results'][backend] = rows
        traces[backend] = (io, copies)
        with (args.directory/(backend+'-intervals.csv')).open('w') as output:
            writer = csv.writer(output)
            writer.writerow(['kind', 'start_ns', 'end_ns', 'thread', 'cuda_stream', 'bytes'])
            for a, b, tid, name in io:
                writer.writerow([name, a, b, tid, '', ''])
            for a, b, kind, stream, tid, size in copies:
                writer.writerow(['D2H' if kind == 2 else 'H2D', a, b, tid, stream, size])
    (args.directory/'overlap.json').write_text(json.dumps(result, indent=2)+'\n')
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.patches import Patch
    fig, axes = plt.subplots(2, 1, figsize=(13, 8), constrained_layout=True)
    for ax, backend in zip(axes, ['uring', 'spdk']):
        io, copies = traces[backend]
        write_ranges = [row for row in io if row[3].endswith('write_wait')]
        origin = write_ranges[3*128][0]  # first instrumented round after 3 warmups
        end = origin+8_000_000
        ids = sorted({row[2] for row in io})
        for lane, tid in enumerate(ids):
            disk = [((a-origin)/1e6, (min(b,end)-a)/1e6) for a,b,t,name in io if t==tid and a<end and b>origin]
            gpu = [((max(a,origin)-origin)/1e6, (min(b,end)-max(a,origin))/1e6) for a,b,kind,stream,t,size in copies if t==tid and kind==2 and a<end and b>origin]
            ax.broken_barh(disk, (lane-.3,.6), facecolors='#f59e0b', alpha=.45)
            ax.broken_barh(gpu, (lane-.2,.4), facecolors='#2563eb')
        ax.set_title(backend+': first 8 ms of measured GPU spill (after 3 warmups, instrumented)')
        ax.set_xlim(0,8)
        ax.set_yticks(range(len(ids)), [str(n) for n in range(len(ids))])
        ax.set_ylabel('Migration worker lane')
        ax.set_xlabel('Milliseconds since first SSD call')
        ax.grid(axis='x',alpha=.2)
        ax.legend(handles=[Patch(color='#f59e0b',alpha=.45,label='Outstanding SSD call (dispatch/copy/wait)'),
                           Patch(color='#2563eb',label='CUDA device D2H execution')],loc='upper right')
    fig.savefig(args.directory/'pipeline-overlap.png',dpi=170)
    fig.savefig(args.directory/'pipeline-overlap.svg')
    print(json.dumps(result,indent=2))


if __name__ == '__main__':
    main()
