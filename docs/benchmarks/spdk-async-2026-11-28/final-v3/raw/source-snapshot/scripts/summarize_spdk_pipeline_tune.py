#!/usr/bin/env python3
"""Audit matched GPU pipeline samples; never add overlapping phase durations."""
import argparse
import csv
import json
import math
from pathlib import Path
import re
import statistics


def dist(a):
    a=sorted(a);assert a and all(math.isfinite(x) and x>=0 for x in a)
    return dict(n=len(a),p50=statistics.median(a),p95=a[math.ceil(.95*len(a))-1],mean=statistics.mean(a),min=a[0],max=a[-1])


def gain(b,s):return (b-s)/b*100


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path)
    args=p.parse_args();out=args.directory
    run=json.loads((out/'run.json').read_text());assert 'error' not in run,run.get('error')
    assert all(r['matches_original'] for r in run['checks']['after'])
    assert run['original_governors']==run['final_governors']
    assert run['initial_state']['driver']==run['final_state']['driver']
    assert run['initial_state']['service']==run['final_state']['service']
    assert len(run['commands'])==81 and all(c['returncode']==0 for c in run['commands'])
    fields=['read_ms','crc_ms','h2d_ms','total_ms','cpu_ms','read_cpu_ms','wait_ms','h2d_submit_ms','final_sync_ms']
    groups={};batches={};warmups=0;raw=[]
    for f in sorted(out.glob('b*-*.csv')):
        m=re.fullmatch(r'b(\d+)-(powersave|performance)-(uring|spdk)-(serial|pipeline)-qd(4|8|16|32)\.csv',f.name);assert m,f
        batch,profile,backend,strategy,qd=int(m[1]),m[2],m[3],m[4],int(m[5])
        rows=list(csv.DictReader(f.open()));assert len(rows)==33
        assert [int(r['round']) for r in rows]==list(range(-3,30))
        assert 'all_rounds_identity_version_crc_cpu_gpu_bytes=PASS' in f.with_suffix('.txt').read_text()
        for r in rows:
            assert (r['backend'],r['target'],r['strategy'])==(backend,'gpu',strategy)
            assert int(r['batch'])==batch and int(r['qd'])==int(r['max_inflight'])==qd
            assert int(r['payload_bytes'])==int(r['verified_bytes'])==16777216
            assert int(r['submitted'])==int(r['completed'])==32
            assert int(r['overlapped'])==(strategy=='pipeline')
            for field in fields:
                r[field]=float(r[field]);assert math.isfinite(r[field]) and r[field]>=0
            if strategy=='serial':
                assert abs(r['total_ms']-sum(r[k] for k in ['read_ms','crc_ms','h2d_ms']))<.00001
            else:
                assert r['read_ms']<=r['total_ms']
            assert int(r['temp_before_c'])>0 and int(r['temp_after_c'])>0
            assert int(r['warmup'])==(int(r['round'])<0)
            if r['warmup']=='1':warmups+=1;continue
            key=f'{profile}-{backend}-{strategy}-qd{qd}'
            groups.setdefault(key,[]).append(r);batches.setdefault(f'b{batch}-'+key,[]).append(r)
            raw.append({'file':f.name,**r})
    assert len(groups)==20 and len(raw)==2400 and warmups==240
    def stats(rows):
        result={k:dist([r[k] for r in rows]) for k in fields}
        result.update(gib_s_at_p50=.015625/(result['total_ms']['p50']/1000),
                      cpu_core_equivalent=sum(r['cpu_ms'] for r in rows)/sum(r['total_ms'] for r in rows),
                      min_temp_c=min(int(r[k]) for r in rows for k in ['temp_before_c','temp_after_c']),
                      max_temp_c=max(int(r[k]) for r in rows for k in ['temp_before_c','temp_after_c']))
        return result
    summary={'samples':2400,'warmups':240,'block_bytes':524288,'blocks':32,'payload_bytes':16777216,
             'quantiles':'P50 median, P95 nearest-rank',
             'pipeline_metric_semantics':{
                 'read_ms':'wall time from launch to final storage completion; overlaps CRC and H2D',
                 'crc_ms':'sum of host record-validation work intervals',
                 'h2d_ms':'host H2D submission time plus final synchronization wait; not full DMA duration',
                 'read_cpu_ms':'compatibility field: process CPU until all CRC and H2D submissions complete; not isolated read CPU',
                 'total_ms':'launch through final CUDA stream synchronization; primary endpoint'},
             'groups':{},'batches':{},'comparisons':{},'best':{}}
    for key,rows in groups.items():assert len(rows)==120;summary['groups'][key]=stats(rows)
    for key,rows in batches.items():assert len(rows)==30;summary['batches'][key]=stats(rows)
    def compare(base,new):
        a=summary['groups'][base];b=summary['groups'][new]
        return {'baseline':base,'new':new,
                **{f'{q}_improvement_pct':gain(a['total_ms'][q],b['total_ms'][q]) for q in ['p50','p95']},
                'cpu_p50_ratio':b['cpu_ms']['p50']/a['cpu_ms']['p50'],
                'batch_p50_improvement_pct':[gain(summary['batches'][f'b{i}-'+base]['total_ms']['p50'],
                                                 summary['batches'][f'b{i}-'+new]['total_ms']['p50']) for i in range(1,5)],
                'batch_p95_improvement_pct':[gain(summary['batches'][f'b{i}-'+base]['total_ms']['p95'],
                                                 summary['batches'][f'b{i}-'+new]['total_ms']['p95']) for i in range(1,5)]}
    for profile in ['powersave','performance']:
        for backend in ['uring','spdk']:
            base=f'{profile}-{backend}-serial-qd4';new=f'{profile}-{backend}-pipeline-qd4'
            summary['comparisons'][f'{profile}-{backend}-pipeline_gain_same_qd']=compare(base,new)
            keys=[k for k in groups if k.startswith(profile+'-'+backend+'-')]
            summary['best'][profile+'-'+backend]=min(keys,key=lambda k:summary['groups'][k]['total_ms']['p50'])
        for qd in [4,8,16,32]:
            summary['comparisons'][f'{profile}-matched-backend-qd{qd}']=compare(f'{profile}-uring-pipeline-qd{qd}',f'{profile}-spdk-pipeline-qd{qd}')
        summary['comparisons'][profile+'-best-backends']=compare(summary['best'][profile+'-uring'],summary['best'][profile+'-spdk'])
    for backend in ['uring','spdk']:
        summary['comparisons'][backend+'-governor-gain-qd4']=compare(f'powersave-{backend}-pipeline-qd4',f'performance-{backend}-pipeline-qd4')
    summary['fastest_spdk']=min([k for k in groups if '-spdk-' in k],key=lambda k:summary['groups'][k]['total_ms']['p50'])
    summary['fastest_uring']=min([k for k in groups if '-uring-' in k],key=lambda k:summary['groups'][k]['total_ms']['p50'])
    summary['process_cpu']={c['name']:{k:c[k] for k in ['process_user_cpu_s','process_system_cpu_s','wall_s']} for c in run['commands'] if c['name']!='prepare'}
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    audit={'largest_total_ms':sorted(raw,key=lambda r:r['total_ms'],reverse=True)[:10],
           'smallest_total_ms':sorted(raw,key=lambda r:r['total_ms'])[:4],
           'removed_samples':0,'source_csv_files':80,'verified_rounds':2640,
           'all_actual_qd_equal_requested':True,'all_original_region_hashes_restored':True,
           'device_service_governors_restored':True}
    (out/'sample-audit.json').write_text(json.dumps(audit,indent=2)+'\n')
    print(json.dumps({'best':summary['best'],'fastest_spdk':summary['fastest_spdk'],'fastest_uring':summary['fastest_uring'],
                      'comparisons':summary['comparisons']},indent=2))


if __name__=='__main__':main()
