#!/usr/bin/env python3
"""Validate raw samples and reproduce all accepted comparison statistics."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import statistics


def distribution(values):
    a=sorted(values)
    assert a and all(math.isfinite(x) and x>=0 for x in a)
    return {'n':len(a),'p50':statistics.median(a),'p95':a[math.ceil(.95*len(a))-1],
            'min':a[0],'max':a[-1]}


def pct(baseline,spdk):
    return (baseline-spdk)/baseline*100


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory',type=Path)
    args=p.parse_args();out=args.directory
    run=json.loads((out/'run.json').read_text())
    assert 'error' not in run,run.get('error')
    assert all(r['matches_original'] for r in run['checks']['after'])
    assert run['final_state']['driver']==run['original_state']['driver']
    groups={};all_rows=[];batch_rows={}
    for f in sorted(out.glob('b*-*-qd*-*.csv')):
        rows=list(csv.DictReader(f.open()))
        assert len(rows)==33
        assert [int(r['round']) for r in rows]==list(range(-3,30))
        for r in rows:
            assert int(r['payload_bytes'])==int(r['verified_bytes'])==16777216
            assert int(r['qd'])==int(r['max_inflight'])
            assert int(r['submitted'])==int(r['completed'])==32
            assert int(r['temp_before_c'])>0 and int(r['temp_after_c'])>0
            for field in ['total_ms','read_ms','crc_ms','h2d_ms','cpu_ms','read_cpu_ms']:
                r[field]=float(r[field]);assert math.isfinite(r[field]) and r[field]>=0
            assert abs(r['total_ms']-sum(r[k] for k in ['read_ms','crc_ms','h2d_ms']))<.00001
            assert int(r['warmup'])==(int(r['round'])<0)
            if r['warmup']=='1':continue
            key=(r['target'],int(r['qd']),r['backend'])
            groups.setdefault(key,[]).append(r)
            batch_rows.setdefault((int(r['batch']),)+key,[]).append(r)
            all_rows.append(r)
    assert len(groups)==8 and len(all_rows)==960
    summary={'payload_bytes':16777216,'quantiles':'P50=median; P95=nearest-rank',
             'warmups_per_batch_group':3,'measured_per_batch_group':30,
             'batches':4,'measured_samples':len(all_rows),'groups':{},'comparison':{},'batches_detail':{}}
    def stats(rows):
        result={k:distribution([r[k] for r in rows]) for k in ['total_ms','read_ms','crc_ms','h2d_ms','cpu_ms','read_cpu_ms']}
        result['p50_gib_s']=.015625/(result['total_ms']['p50']/1000)
        result['p95_latency_gib_s']=.015625/(result['total_ms']['p95']/1000)
        result['cpu_sum_ms']=sum(r['cpu_ms'] for r in rows)
        result['cpu_core_equivalent']=sum(r['cpu_ms'] for r in rows)/sum(r['total_ms'] for r in rows)
        result['temp_min_c']=min(int(r[k]) for r in rows for k in ['temp_before_c','temp_after_c'])
        result['temp_max_c']=max(int(r[k]) for r in rows for k in ['temp_before_c','temp_after_c'])
        return result
    for key,rows in groups.items():
        assert len(rows)==120
        summary['groups']['%s-qd%d-%s'%key]=stats(rows)
    for key,rows in batch_rows.items():
        assert len(rows)==30
        summary['batches_detail']['b%d-%s-qd%d-%s'%key]=stats(rows)
    for target in ['gpu','cpu']:
        for qd in [1,4]:
            base=summary['groups'][f'{target}-qd{qd}-uring'];spdk=summary['groups'][f'{target}-qd{qd}-spdk']
            gains={q:pct(base['total_ms'][q],spdk['total_ms'][q]) for q in ['p50','p95']}
            gains['cpu_p50_extra_ms']=spdk['cpu_ms']['p50']-base['cpu_ms']['p50']
            gains['cpu_p50_ratio']=spdk['cpu_ms']['p50']/base['cpu_ms']['p50']
            gains['batches']={}
            for batch in range(1,5):
                b=summary['batches_detail'][f'b{batch}-{target}-qd{qd}-uring']
                s=summary['batches_detail'][f'b{batch}-{target}-qd{qd}-spdk']
                gains['batches'][str(batch)]={q:pct(b['total_ms'][q],s['total_ms'][q]) for q in ['p50','p95']}
            summary['comparison'][f'{target}-qd{qd}']=gains
    for target in ['gpu','cpu']:
        summary['comparison'][target+'-best']={backend:min([1,4],key=lambda q:summary['groups'][f'{target}-qd{q}-{backend}']['total_ms']['p50']) for backend in ['uring','spdk']}
    infer=[];outputs=set()
    for f in sorted(out.glob('infer-b*-*.txt')):
        m=re.fullmatch(r'infer-b(\d+)-(off|spdk)-(\d+)\.txt',f.name);assert m
        batch,mode,index=int(m[1]),m[2],int(m[3])
        log=f.read_text()
        ttft=re.findall(r'^request=0 ttft_ns=(\d+)',log,re.M)
        counters=re.findall(r'^generated=(\d+) forwards=(\d+) reused=(\d+) preemptions=(\d+).*prefilled=(\d+)',log,re.M)
        ids=re.findall(r'^\[0\] (.*)$',log,re.M)
        assert len(ttft)==len(counters)==len(ids)==3
        assert ids[0]==ids[2]
        outputs.add(tuple(ids))
        reads=re.findall(r'ssd_reads=(\d+) ssd_writes=(\d+)',log)
        delta=int(reads[2][0])-int(reads[1][0]) if mode=='spdk' else 0
        writes=int(reads[2][1])-int(reads[1][1]) if mode=='spdk' else 0
        reused=int(counters[2][2]);prefilled=int(counters[2][4])
        assert (delta,reused,prefilled)==((1,32,2) if mode=='spdk' else (0,0,34))
        row={'batch':batch,'mode':mode,'round':index-3,'warmup':int(index<3),
             'ttft_ms':int(ttft[2])/1e6,'ssd_reads':delta,'ssd_writes':writes,
             'ssd_restored_tokens':delta*8,'resident_reused_tokens':reused-delta*8,
             'total_reused_tokens':reused,'prefilled_tokens':prefilled,'output_token_ids':ids[2]}
        infer.append(row)
    assert len(outputs)==1 and len(infer)==78
    with (out/'inference.csv').open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(infer[0]));w.writeheader();w.writerows(infer)
    summary['inference']={'uring':{'status':'not connected to raw O_DIRECT SSD region; no accepted TTFT samples'},'groups':{}}
    for mode in ['off','spdk']:
        rows=[r for r in infer if r['mode']==mode and not r['warmup']]
        assert len(rows)==30
        summary['inference']['groups'][mode]={'ttft_ms':distribution([r['ttft_ms'] for r in rows]),
           'batches':{str(b):distribution([r['ttft_ms'] for r in rows if r['batch']==b]) for b in range(1,4)},
           **{k:rows[0][k] for k in ['ssd_reads','ssd_restored_tokens','resident_reused_tokens','total_reused_tokens','prefilled_tokens','output_token_ids']}}
    b=summary['inference']['groups']['off']['ttft_ms'];s=summary['inference']['groups']['spdk']['ttft_ms']
    summary['inference']['spdk_offload_vs_off_improvement_pct']={q:pct(b[q],s[q]) for q in ['p50','p95']}
    summary['inference']['spdk_vs_uring_extra_improvement_pct']=None
    legacy=[line.split(',') for line in (out/'historical-cuda-replay.txt').read_text().splitlines() if line.startswith('cuda-spdk,')]
    assert len(legacy)==30
    with (out/'historical-cuda-replay.csv').open('w') as f:
        f.write('mode,round,payload_bytes,spill_ms,recovery_ms,verified_bytes\n')
        for row in legacy:
            assert row[2]==row[5]=='16777216';f.write(','.join(row)+'\n')
    summary['historical_replay_ms']=distribution([float(r[4]) for r in legacy])
    summary['historical_original_p50_ms']=18.9515095
    summary['restoration_verified']=True
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps({'comparison':summary['comparison'],'inference':summary['inference'],'historical_replay_ms':summary['historical_replay_ms']},indent=2))


if __name__=='__main__':main()
