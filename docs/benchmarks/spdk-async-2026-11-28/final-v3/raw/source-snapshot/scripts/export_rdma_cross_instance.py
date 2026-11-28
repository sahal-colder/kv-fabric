#!/usr/bin/env python3
"""Publish only reviewed numeric evidence. Full raw traces and KV stay local.

Inputs are produced by bench/summarize_rdma_cross_instance.py. No recursive
copy of source logs, snapshots, models, env, process metadata, or credentials.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path);p.add_argument('out',type=Path)
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True);items=[]
    def copy(source,dest):
        dst=a.out/dest;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,dst)
        items.append(dict(path=dest,bytes=dst.stat().st_size,sha256=hashlib.sha256(dst.read_bytes()).hexdigest(),
                          source=str(source.relative_to(a.archive))))
    copy(a.archive/'report/summary.json','summary.json')
    for label,folder in [('reference','accepted-siw'),('ablation','final-rdma'),('final','verified-final-rdma')]:
        for mode in (('cold','cpu-hot','baseline') if label=='reference' else ('direct','optimized') if label=='ablation' else
                     ('optimized','fault-delay','fault-cut','recovery')):
            base=a.archive/folder/mode
            for name in ('requests.jsonl','kvfabric.jsonl','summary.json','fault-verification.json'):
                if (base/name).exists():copy(base/name,f'{label}/{mode}/{name}')
        if label in ('ablation','final'):
            for mode in (('baseline','optimized')if label=='ablation'else('optimized',)):
                base=a.archive/folder/('profile-'+mode)
                for name in ('requests.jsonl','kvfabric.jsonl','summary.json','attribution.json'):
                    if (base/name).exists():copy(base/name,f'{label}/profile-{mode}/{name}')
    for log in sorted((a.archive/'ceiling-siw').glob('*.log')):
        if log.name.startswith(('ceiling-r','rstream-r')):copy(log,'ceiling/'+log.name)
    for base in sorted((a.archive/'tuning-confirm-siw').iterdir()):
        if base.is_dir():
            for name in ('data-results.json','rejected.json'):
                if (base/name).exists():copy(base/name,'tuning/'+base.name+'/'+name)
    for name in ('ctest-delivery.log','snapshot-delivery.log','snapshot-bounds.log','python-delivery.log','public-ctest.log'):
        if (a.archive/name).exists():
            # Project-relative paths in assertions; no host-directory export.
            text=(a.archive/name).read_text().replace(str(ROOT)+'/','').replace(str(ROOT),'REPO')
            dst=a.out/'validation'/name;dst.parent.mkdir(parents=True,exist_ok=True);dst.write_text(text)
            items.append(dict(path=str(dst.relative_to(a.out)),bytes=dst.stat().st_size,
                         sha256=hashlib.sha256(dst.read_bytes()).hexdigest(),source=name))
    failures=[]
    # Explicitly retain the incomplete original baseline and blocked-close history.
    for folder in ('env-probe','env-probe2','env-probe3','siw-probe','siw-patched-probe2',
                   'full-siw','final-siw','measured-siw','accepted-siw','final-rdma','final72-rdma'):
        base=a.archive/folder
        entry=dict(run=folder)
        if (base/'cleanup.txt').exists():entry['cleanup']=(base/'cleanup.txt').read_text()
        if (base/'fault-cut/requests.jsonl').exists():
            entry['fault_cut_requests']=json.loads('['+','.join((base/'fault-cut/requests.jsonl').read_text().splitlines())+']')
        failures.append(entry)
    reasons={
        'env-probe':'Runtime exclusive switch rejected with EBUSY.',
        'env-probe2':'RXE not visible in target namespace; cleanup restoration subsequently corrected.',
        'env-probe3':'Stock RXE namespace migration rejected with EOPNOTSUPP.',
        'siw-probe':'Stock SIW namespace migration rejected with EOPNOTSUPP.',
        'siw-patched-probe2':'Two-namespace SIW functional probe passed.',
        'full-siw':'Absolute Swap guard aborted startup; no TTFT distribution accepted.',
        'final-siw':'Incremental Swap guard aborted startup; no TTFT distribution accepted.',
        'measured-siw':'Swap guard aborted startup; four verified 72MiB source windows retained, no TTFT distribution accepted.',
        'accepted-siw':'1/15 baseline I/O fallback retained in TTFT; later RDMA retry limited to safe read-only cases.',
        'final-rdma':'18MiB chunk ablation retained; provider fault deadline problem observed.',
        'final72-rdma':'72MiB healthy run passed; connection acquire stalled 135399.155523ms under loss before bounded connect tasks.'}
    for entry in failures: entry['reason']=reasons[entry['run']]
    (a.out/'failure-history.json').write_text(json.dumps(failures,indent=2)+'\n')
    if (a.archive/'source-final-sha256.json').exists():
        copy(a.archive/'source-final-sha256.json','source-final-sha256.json')
    raw=[]
    for folder in ('final-rdma','final72-rdma','verified-final-rdma'):
        for f in sorted((a.archive/folder).glob('profile-*/trace.*')):
            raw.append(dict(path=str(f.relative_to(a.archive)),bytes=f.stat().st_size,
                        sha256=hashlib.sha256(f.read_bytes()).hexdigest()))
    (a.out/'local-raw-sha256.json').write_text(json.dumps(raw,indent=2)+'\n')
    (a.out/'evidence-manifest.json').write_text(json.dumps(items,indent=2)+'\n')
    print(len(items),'whitelisted evidence files')
if __name__=='__main__':main()
