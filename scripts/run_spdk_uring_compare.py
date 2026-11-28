#!/usr/bin/env python3
"""Physical Samsung-only experiment; run as root after building the target.

Requires the previously accepted backup manifest. Never formats/partitions.
Every exit after a successful original-data check restores all scratch bytes,
verifies all three regions by O_DIRECT, and restores the original service state.
"""
import argparse
import csv
from datetime import datetime
import hashlib
import importlib.util
import json
import mmap
import os
from pathlib import Path
import signal
import subprocess
import time
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[1]
SERIAL = 'S4Y4NX0R755035'
SERVICE = 'kvfabric-spdk-samsung.service'
LOCAL = ROOT/'.kvfabric-local/spdk-samsung'
BUILD = ROOT/'build-backend-compare'
BENCH = BUILD/'kvf_backend_compare'


def now():
    return datetime.now(ZoneInfo('Australia/Perth')).isoformat()


def command(args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def kernel_disk():
    found = []
    for p in Path('/sys/class/nvme').glob('nvme*'):
        if (p/'serial').read_text().strip() == SERIAL:
            disks = [q for q in p.glob(p.name+'n*') if (q/'dev').is_file()]
            assert len(disks) == 1
            assert (p/'device').resolve().name == '0000:07:00.0'
            assert (p/'model').read_text().strip() == 'SAMSUNG MZVLQ512HALU-00000'
            found.append('/dev/'+disks[0].name)
    assert len(found) == 1, 'target serial not uniquely visible through kernel'
    return found[0]


def state():
    return {'time': now(), 'driver': Path('/sys/bus/pci/devices/0000:07:00.0/driver').resolve().name,
            'service': command(['systemctl','show',SERVICE,'-p','ActiveState','-p','SubState','-p','UnitFileState'], capture_output=True,text=True).stdout}


def direct_hash(device, region):
    h = hashlib.sha256()
    with mmap.mmap(-1, 4*1024*1024) as buf:
        fd = os.open(device, os.O_RDONLY|os.O_DIRECT)
        try:
            for pos in range(0, region['length'], len(buf)):
                n = min(len(buf), region['length']-pos)
                view = memoryview(buf)[:n]
                assert os.preadv(fd,[view],region['offset']+pos) == n
                h.update(view)
                view.release()
        finally:
            os.close(fd)
    return h.hexdigest()


def restore(device, region):
    fd = os.open(device,os.O_RDWR|os.O_DIRECT)
    try:
        with open(region['backup'],'rb') as source, mmap.mmap(-1,4*1024*1024) as buf:
            for pos in range(0,region['length'],len(buf)):
                n = min(len(buf),region['length']-pos)
                view = memoryview(buf)[:n]
                assert source.readinto(view) == n
                assert os.pwritev(fd,[view],region['offset']+pos) == n
                view.release()
            os.fsync(fd)
    finally:
        os.close(fd)
    command(['blockdev','--flushbufs',device])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--batches',type=int,default=4)
    p.add_argument('--smoke',action='store_true')
    p.add_argument('--restore-initial-active',action='store_true',
                   help='initial preflight already stopped the originally active service')
    args=p.parse_args()
    assert os.geteuid()==0
    assert 1<=args.batches<=8
    out=args.out.resolve(); out.mkdir(parents=True,exist_ok=False)
    manifest=json.loads((LOCAL/'backup-manifest.json').read_text())
    assert manifest['serial']==SERIAL
    assert manifest['device_bytes']==512110190592
    scratch=next(r for r in manifest['regions'] if r['name']=='scratch')
    assert (scratch['offset'],scratch['length'])==(1073741824,134217728)
    initial=state()
    active=args.restore_initial_active or 'ActiveState=active\n' in initial['service']
    metadata={'started':now(),'observed_at_runner_start':initial,
              'original_state':{'driver':'vfio-pci' if active else initial['driver'],
                                'service_active':active,'service_enabled':True},
              'serial':SERIAL,'manifest':manifest,'commands':[], 'checks':{}}
    def save():
        (out/'run.json').write_text(json.dumps(metadata,indent=2)+'\n')
    def execute(name,cmd,**kw):
        metadata['commands'].append({'name':name,'time':now(),'argv':list(map(str,cmd))});save()
        with (out/(name+'.txt')).open('w') as log:
            start=time.monotonic()
            result=subprocess.run(list(map(str,cmd)),stdout=log,stderr=subprocess.STDOUT,timeout=600,**kw)
        metadata['commands'][-1].update(returncode=result.returncode,wall_s=time.monotonic()-start)
        save(); assert result.returncode==0, name
        print(name+': PASS',flush=True)
    verified_original=False
    def interrupted(signum,frame):
        raise RuntimeError('interrupted signal '+str(signum))
    signal.signal(signal.SIGTERM,interrupted)
    signal.signal(signal.SIGINT,interrupted)
    try:
        command(['systemctl','stop',SERVICE])
        device=kernel_disk()
        spec=importlib.util.spec_from_file_location('spdk_device',ROOT/'scripts/spdk_device.py')
        helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
        helper.unused(Path('/sys/class/block')/Path(device).name)
        assert int((Path('/sys/class/block')/Path(device).name/'size').read_text())*512==manifest['device_bytes']
        checks=[]
        for region in manifest['regions']:
            backup=Path(region['backup'])
            assert backup.stat().st_size==region['length']
            expected=hashlib.sha256(backup.read_bytes()).hexdigest()
            actual=direct_hash(device,region)
            assert expected==actual==region['sha256'], region['name']+' original mismatch'
            checks.append({'name':region['name'],'backup_sha256':expected,'raw_direct_sha256':actual})
        metadata['checks']['before']=checks; verified_original=True; save()
        execute('environment',['bash','-c',
            'uname -a; lscpu; nvidia-smi -q; lsblk -o NAME,MODEL,SERIAL,SIZE,TYPE,MOUNTPOINTS; '
            'nvme id-ctrl '+device+'; nvme smart-log '+device+'; '
            'cat /sys/class/block/'+Path(device).name+'/queue/max_sectors_kb; '
            'cat /sys/devices/system/cpu/cpu{2,4}/cpufreq/scaling_governor; '
            'git rev-parse HEAD; git status --short'])
        execute('prepare',[BENCH,'prepare',device,1,'cpu','unused',30,3,0])
        for batch in range(1,args.batches+1):
            order=['uring','spdk'] if batch%2 else ['spdk','uring']
            for backend in order:
                command(['systemctl','start' if backend=='spdk' else 'stop',SERVICE])
                target='/etc/kvfabric/spdk-samsung.json' if backend=='spdk' else kernel_disk()
                configs=[(1,'gpu'),(4,'gpu'),(1,'cpu'),(4,'cpu')]
                if batch%2==0: configs.reverse()
                if args.smoke: configs=[(4,'gpu')]
                for qd,dest in configs:
                    name=f'b{batch}-{backend}-qd{qd}-{dest}'
                    execute(name,[BENCH,backend,target,qd,dest,out/(name+'.csv'),30,3,batch])
        # Historical end-to-end sequential migration, separate from fixed-record
        # read-only comparison. It writes within the same backed-up region.
        if not args.smoke:
            command(['systemctl','start',SERVICE])
            legacy=[BUILD/'bin/kvf_spdk_verify','/etc/kvfabric/spdk-samsung.json','Samsungn1',1073741824,134217728]
            execute('historical-cuda-replay',legacy+['cuda','--rounds',30,'--warmup',3])
            spec=importlib.util.spec_from_file_location('fixture',ROOT/'tests/nanovllm/test_remote_inference.py')
            fixture=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixture)
            fixture_dir=LOCAL/'compare-fixture';fixture_dir.mkdir(exist_ok=True)
            model,tokenizer=fixture.fixtures(fixture_dir)
            metadata['fixture_sha256']={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in (model,tokenizer)}
            common=[BUILD/'bin/miniinfer',model,'-z',tokenizer,'--device','cuda','--stdin',
                    '--blocks',8,'--block-size',8,'--batch',1,'-n',4,'-t',0,'--ids','--ignore-eos','--trace']
            prompts='abcdefghijklmnopabcdefghijklmnop\nZYXWVUTSRQPONMLKZYXWVUTSRQPONMLK\nabcdefghijklmnopabcdefghijklmnop\n'
            for batch in range(1,4):
                for mode in (['off','spdk'] if batch%2 else ['spdk','off']):
                    extra=[] if mode=='off' else ['--ssd-mode','spdk','--ssd-slots',32,
                       '--spdk-config','/etc/kvfabric/spdk-samsung.json','--spdk-bdev','Samsungn1',
                       '--spdk-offset',1073741824,'--spdk-reactor-mask','0x4',
                       '--spdk-iova-mode','va','--spdk-base-virtaddr','0x1000000000']
                    for n in range(-3,10):
                        # A fresh process has empty RAM/SSD prefix indices each time.
                        name=f'infer-b{batch}-{mode}-{n+3:02d}'
                        execute(name,['taskset','-c','4']+common+extra,input=prompts,text=True)
            metadata['inference_uring_gap']='Current engine supports buffered temporary files only; no raw O_DIRECT region adapter. No same-disk io_uring TTFT claim.'
    except BaseException as error:
        metadata['error']=repr(error)
        raise
    finally:
        # Never overwrite unexpected original data when the initial check failed.
        if verified_original:
            command(['systemctl','stop',SERVICE]);device=kernel_disk()
            restore(device,scratch)
            metadata['checks']['after']=[]
            for region in manifest['regions']:
                actual=direct_hash(device,region)
                metadata['checks']['after'].append({'name':region['name'],'raw_direct_sha256':actual,'matches_original':actual==region['sha256']})
                assert actual==region['sha256'], 'RESTORE FAILED: '+region['name']
        command(['systemctl','start' if active else 'stop',SERVICE])
        metadata['final_state']=state();metadata['finished']=now();save()
        command(['chown','-R','wty:wty',out])
        print('restored original bytes, hashes and service state',flush=True)


if __name__=='__main__':
    main()
