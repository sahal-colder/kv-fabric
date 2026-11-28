#!/usr/bin/env python3
"""Samsung-only tuning: matched backends, QD, pipeline, CPU governor, restore."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import time

import run_spdk_uring_compare as safety


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--batches',type=int,default=4)
    p.add_argument('--smoke',action='store_true')
    args=p.parse_args();assert os.geteuid()==0 and 1<=args.batches<=8
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    root=safety.ROOT;manifest=json.loads((safety.LOCAL/'backup-manifest.json').read_text())
    assert manifest['serial']==safety.SERIAL and manifest['device_bytes']==512110190592
    scratch=next(r for r in manifest['regions'] if r['name']=='scratch')
    assert (scratch['offset'],scratch['length'])==(1073741824,134217728)
    initial=safety.state();active='ActiveState=active\n' in initial['service']
    governors={str(Path(f'/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor').resolve()):
               Path(f'/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor').read_text().strip() for cpu in [2,4]}
    meta={'started':safety.now(),'initial_state':initial,'original_governors':governors,
          'manifest':manifest,'commands':[],'checks':{},'smoke':args.smoke}
    def save(): (out/'run.json').write_text(json.dumps(meta,indent=2)+'\n')
    def execute(name,argv):
        r0=resource.getrusage(resource.RUSAGE_CHILDREN);started=time.monotonic()
        entry={'name':name,'time':safety.now(),'argv':list(map(str,argv))};meta['commands'].append(entry);save()
        with (out/(name+'.txt')).open('w') as log:
            proc=subprocess.run(list(map(str,argv)),stdout=log,stderr=subprocess.STDOUT,timeout=180)
        r1=resource.getrusage(resource.RUSAGE_CHILDREN)
        entry.update(returncode=proc.returncode,wall_s=time.monotonic()-started,
                     process_user_cpu_s=r1.ru_utime-r0.ru_utime,process_system_cpu_s=r1.ru_stime-r0.ru_stime)
        save();assert proc.returncode==0,name
        print(name+': PASS',flush=True)
    def interrupt(signum,frame):raise RuntimeError('interrupted '+str(signum))
    signal.signal(signal.SIGTERM,interrupt);signal.signal(signal.SIGINT,interrupt)
    verified=False
    try:
        safety.command(['systemctl','stop',safety.SERVICE]);device=safety.kernel_disk()
        spec=importlib.util.spec_from_file_location('device',root/'scripts/spdk_device.py')
        helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
        helper.unused(Path('/sys/class/block')/Path(device).name)
        meta['checks']['before']=[]
        for region in manifest['regions']:
            backup=Path(region['backup']);assert backup.stat().st_size==region['length']
            digest=hashlib.sha256(backup.read_bytes()).hexdigest();actual=safety.direct_hash(device,region)
            assert digest==actual==region['sha256'],region['name']
            meta['checks']['before'].append({'name':region['name'],'backup_sha256':digest,'raw_direct_sha256':actual})
        verified=True;save()
        execute('prepare',[safety.BENCH,'prepare',device,4,'gpu','unused',30,3,0])
        for batch in range(1,args.batches+1):
            profiles=['powersave'] if args.smoke else (['powersave','performance'] if batch%2 else ['performance','powersave'])
            for profile in profiles:
                for path in governors:Path(path).write_text(profile)
                for backend in (['uring','spdk'] if batch%2 else ['spdk','uring']):
                    safety.command(['systemctl','start' if backend=='spdk' else 'stop',safety.SERVICE])
                    target='/etc/kvfabric/spdk-samsung.json' if backend=='spdk' else safety.kernel_disk()
                    configs=[('serial',4),('pipeline',4)] if args.smoke else [('serial',4),('pipeline',4),('pipeline',8),('pipeline',16),('pipeline',32)]
                    if batch%2==0:configs.reverse()
                    for strategy,qd in configs:
                        name=f'b{batch}-{profile}-{backend}-{strategy}-qd{qd}'
                        execute(name,[safety.BENCH,backend,target,qd,'gpu',out/(name+'.csv'),30,3,batch,strategy])
        meta['source_commit']=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
    except BaseException as e:
        meta['error']=repr(e);raise
    finally:
        try:
            if verified:
                safety.command(['systemctl','stop',safety.SERVICE]);device=safety.kernel_disk()
                safety.restore(device,scratch);meta['checks']['after']=[]
                for region in manifest['regions']:
                    actual=safety.direct_hash(device,region)
                    meta['checks']['after'].append({'name':region['name'],'raw_direct_sha256':actual,'matches_original':actual==region['sha256']})
                    assert actual==region['sha256'],'RESTORE FAILED '+region['name']
        finally:
            for path,value in governors.items():Path(path).write_text(value)
            safety.command(['systemctl','start' if active else 'stop',safety.SERVICE])
            meta['final_state']=safety.state()
            meta['final_governors']={path:Path(path).read_text().strip() for path in governors}
            meta['finished']=safety.now();save();safety.command(['chown','-R','wty:wty',out])
            print('original data, governor and device state restored',flush=True)


if __name__=='__main__':main()
