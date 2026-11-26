#!/usr/bin/env python3
"""Reconcile request JSONL, payload counters, raw perftest and NSys GPU intervals."""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import re
import sqlite3
import statistics
import sys
from summarize_nsys import merge, intersections, difference, duration

GIB=1 << 30
PAYLOAD=288 << 20

def rows(path):
    return [json.loads(x) for x in path.read_text().splitlines()] if path.exists() else []

def group(path, reference):
    requests=rows(path/'requests.jsonl')
    assert requests
    assert [r['output_ids']for r in requests]==[r['output_ids']for r in reference[:len(requests)]]
    assert [r['prompt_sha256']for r in requests]==[r['prompt_sha256']for r in reference[:len(requests)]]
    events=[e for e in rows(path/'kvfabric.jsonl') if any(
        e.get('request_id','').startswith(r['request_id']+'-') for r in requests)]
    loads=[e for e in events if e['event']=='load_total' and e['status'] is None]
    hit_tokens=sum(e.get('tokens',0)for e in events if e['event']=='hit')
    times=sorted(r['ttft_ms']for r in requests)
    result=dict(requests=len(requests),ttft_p50_ms=statistics.median(times),
        ttft_p95_ms=times[math.ceil(.95*len(times))-1],tokens_match=True,
        measured_hit_tokens=hit_tokens,load_failures=sum(e['event']=='load_failure'for e in events))
    if loads:
        assert all(e['payload_bytes']==PAYLOAD for e in loads)
        ms=statistics.median(e['end_ns']-e['begin_ns']for e in loads)/1e6
        result.update(successful_imports=len(loads),import_p50_ms=ms,
                      goodput_gib_s=PAYLOAD/GIB/(ms/1000))
    return result


def profile(path):
    database=path/'trace.sqlite'
    requests=rows(path/'requests.jsonl')
    with sqlite3.connect(str(database)) as db:
        groups=defaultdict(list)
        for lo,hi,tid,name in db.execute('''SELECT n.start,n.end,n.globalTid,coalesce(n.text,s.value)
            FROM NVTX_EVENTS n LEFT JOIN StringIds s ON s.id=n.textId WHERE n.end>n.start'''):
            groups[name].append((lo,hi,tid))
        tables={x[0]for x in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        api=[]
        for table in ('CUPTI_ACTIVITY_KIND_RUNTIME','CUPTI_ACTIVITY_KIND_DRIVER'):
            if table in tables:api.extend(db.execute(f'SELECT start,end,globalTid,correlationId FROM {table}'))
        copies=db.execute('SELECT start,end,bytes,copyKind,correlationId FROM CUPTI_ACTIVITY_KIND_MEMCPY').fetchall()
        kernels=db.execute('''SELECT k.start,k.end,s.value,k.correlationId FROM CUPTI_ACTIVITY_KIND_KERNEL k
                              LEFT JOIN StringIds s ON s.id=k.demangledName''').fetchall()
    def named(names,window):
        return intersections(window,merge((lo,hi)for name in names for lo,hi,_ in groups[name]))
    def correlations(name):
        bytid=defaultdict(list)
        for lo,hi,tid in groups[name]:bytid[tid].append((lo,hi))
        return {corr for lo,hi,tid,corr in api if tid in bytid and any(lo<b and hi>a for a,b in bytid[tid])}
    prefill_ids=set()
    for name in groups:
        if re.match(r'execute_context_\d+\(2048\)',name): prefill_ids |= correlations(name)
    suffix_kernels=merge((lo,hi)for lo,hi,name,corr in kernels if corr in prefill_ids)
    h2d_ids=correlations('kvfabric.h2d')
    h2d=merge((lo,hi)for lo,hi,n,kind,corr in copies if kind==1 and corr in h2d_ids)
    scatter=merge((lo,hi)for lo,hi,name,_ in kernels if name and 'scatter_kv_pages' in name)
    model=merge((lo,hi)for lo,hi,name,_ in kernels if name and 'scatter_kv_pages' not in name and 'gather_kv_pages' not in name)
    spans=sorted(groups['rdma.client.ttft'])
    assert len(spans)==len(requests),(len(spans),len(requests))
    results=[]
    for i,((lo,hi,_),request)in enumerate(zip(spans,requests)):
        window=[(lo,hi)]
        meta=named(('kvfabric.lookup','kvfabric.rdma.acquire','kvfabric.rdma.finish'),window)
        wire=named(('kvfabric.wire.header_wait','kvfabric.wire.body_receive'),window)
        body=named(('kvfabric.wire.body_receive',),window)
        crc=named(('kvfabric.wire.payload_crc','kvfabric.rdma.manifest_crc'),window)
        gpu=intersections(window,merge(h2d+scatter));mdl=intersections(window,model)
        load=named(('kvfabric.load.total',),window)
        overlap=intersections(body,gpu)
        fetch_gpu_overlap=intersections(named(('kvfabric.rdma.direct_receive',),window),gpu)
        # Disjoint critical-wall partition: hidden GPU work is charged to wire
        # wall. GPU active/overlap are reported separately, never added twice.
        suffix=intersections(window,suffix_kernels)
        occupied=[];parts={}
        for label,sp in [('metadata',meta),('wire_wait_and_receive',wire),('crc',crc),
                         ('h2d_scatter_exposed',gpu),('suffix_prefill_gpu_active',suffix),
                         ('other_model_gpu_active',mdl)]:
            exposed=difference(sp,occupied)
            parts[label]=duration(exposed)/1e6
            occupied=merge(occupied+sp)
        parts['other_scheduler_host_gaps']=duration(difference(window,occupied))/1e6
        wall=(hi-lo)/1e6
        assert abs(sum(parts.values())-wall)<1e-6
        bytes=sum(n for a,b,n,kind,corr in copies if kind==1 and corr in h2d_ids and a>=lo and b<=hi)
        assert bytes==PAYLOAD,(i,bytes)
        active=duration(gpu)
        suffix=intersections(window,suffix_kernels)
        results.append(dict(index=i,client_ttft_ms=request['ttft_ms'],nvtx_ttft_ms=wall,
            instrumentation_boundary_ms=wall-request['ttft_ms'],phase_ms=parts,
            suffix_prefill_gpu_active_ms=duration(suffix)/1e6,
            suffix_prefill_gpu_span_ms=(suffix[-1][1]-suffix[0][0])/1e6 if suffix else None,
            suffix_prefill_count=sum(len(intersections(window,[(a,b)]))for name in groups
                if re.match(r'execute_context_\d+\(2048\)',name)for a,b,_ in groups[name]),
            import_wall_ms=duration(load)/1e6,h2d_scatter_active_ms=active/1e6,
            receive_gpu_overlap_ms=duration(overlap)/1e6,
            h2d_hidden_by_fetch_ms=duration(fetch_gpu_overlap)/1e6,
            h2d_hidden_by_fetch_percent=100*duration(fetch_gpu_overlap)/active if active else 0,
            gpu_hidden_percent=100*duration(overlap)/active if active else 0,
            receive_overlap_percent=100*duration(overlap)/duration(body) if duration(body) else 0,
            h2d_payload_bytes=bytes,
            received_payload_ms=duration(body)/1e6,
            wire_goodput_gib_s=PAYLOAD/GIB/(duration(body)/1e9),
            timeline=dict(origin=lo,receive=[(a-lo,b-lo)for a,b in body],
                          h2d=[(a-lo,b-lo)for a,b in intersections(window,h2d)],
                          scatter=[(a-lo,b-lo)for a,b in intersections(window,scatter)])))
    return results


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference-dir',type=Path,required=True)
    p.add_argument('--after-dir',type=Path,required=True)
    p.add_argument('--ablation-dir',type=Path,required=True)
    p.add_argument('--ceiling-dir',type=Path,required=True)
    p.add_argument('--tuning-dir',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();reference=rows(a.reference_dir/'cold/requests.jsonl');assert len(reference)==15
    groups={mode:group(base/folder,reference)for mode,base,folder in (
        ('cold',a.reference_dir,'cold'),('cpu_hot',a.reference_dir,'cpu-hot'),
        ('baseline',a.reference_dir,'baseline'),('direct_tuned',a.ablation_dir,'direct'),
        ('pipeline_tuned',a.after_dir,'optimized'))}
    ceiling={}
    for size in (16384,4<<20,288<<20):
        values=[]
        for f in sorted(a.ceiling_dir.glob(f'ceiling-r?-{size}.log')):
            m=re.search(r'^\s*'+str(size)+r'\s+\d+\s+[\d.]+\s+([\d.]+)',f.read_text(),re.M)
            assert m,f
            values.append(float(m[1])/1024)
        assert len(values)==3
        ceiling[str(size)]=dict(rounds_gib_s=values,p50_gib_s=statistics.median(values))
    anchor=ceiling[str(PAYLOAD)]['p50_gib_s']
    for name,g in groups.items():
        if name != 'cpu_hot' and 'goodput_gib_s'in g:
            g['ceiling_utilization_percent']=100*g['goodput_gib_s']/anchor
    before=groups['baseline'];after=groups['pipeline_tuned'];cold=groups['cold']
    result=dict(hardware='RTX 4060 Laptop 8GiB; single Ubuntu host; two exclusive netns; patched SIW software iWARP',
        groups=groups,ceiling=ceiling,
        ttft_reduction_vs_cold_percent=100*(cold['ttft_p50_ms']-after['ttft_p50_ms'])/cold['ttft_p50_ms'],
        ttft_reduction_vs_baseline_percent=100*(before['ttft_p50_ms']-after['ttft_p50_ms'])/before['ttft_p50_ms'],
        import_reduction_percent=100*(before['import_p50_ms']-after['import_p50_ms'])/before['import_p50_ms'],
        tuning={f.parent.name:json.loads(f.read_text())for f in a.tuning_dir.glob('*/data-results.json')},
        rejected_tuning={f.parent.name:json.loads(f.read_text())for f in a.tuning_dir.glob('*/rejected.json')},
        profiles={'baseline_tuned_serial':profile(a.ablation_dir/'profile-baseline'),
                  'pipeline_tuned':profile(a.after_dir/'profile-optimized')},
        faults={kind:json.loads((a.after_dir/('fault-'+kind)/'fault-verification.json').read_text())for kind in ('delay','cut')},
        recovery=group(a.after_dir/'recovery',reference))
    a.out.mkdir(parents=True,exist_ok=True)
    (a.out/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items()if k not in ('tuning','profiles')},indent=2))
if __name__=='__main__':main()
