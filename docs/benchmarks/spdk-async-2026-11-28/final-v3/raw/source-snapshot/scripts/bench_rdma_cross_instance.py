#!/usr/bin/env python3
"""Two-network-namespace Qwen KV experiment; no TCP/RDMA speed comparison.

The shell wrapper owns privileges, namespace lifetime and cleanup. Source GPU
prefill and destination inference are time-multiplexed on the one 8-GiB GPU.
Only the measured destination owns GPU memory; the live source owns real BF16
snapshots in CPU Storage, not synthetic data. Startup is outside request TTFT.
"""
import argparse
from contextlib import ExitStack
import ctypes as C
import json
import os
from pathlib import Path
import random
import signal
import shutil
import hashlib
import statistics
import subprocess
import sys
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'scripts'), str(ROOT / 'integrations/vllm'),str(ROOT/'tests/vllm')]
from bench_vllm_kv import (HTTP, generate, get, kvfabric_config, process, read_events,
                           reset_gpu_cache, server, token_sources,
                           wait_kvf_idle, wait_kvf_publication)
from kvfabric_vllm.bridge import Bridge, Spec


def workload(rounds):
    rng = random.Random(123 + 16384 * 100000 + 8192)
    tokens = lambda n: [100 + rng.randrange(20000) for _ in range(n)]
    prefix = tokens(8192)
    return prefix, [prefix + tokens(8192) for _ in range(rounds + 2)]


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def worker(a):
    import nvtx
    a.out.mkdir(parents=True, exist_ok=True)
    prefix, prompts = workload(a.rounds)
    metrics = a.out / 'kvfabric.jsonl'
    extra = dict(library=str(a.build / 'src/integration/libkvf_vllm.so'), control=a.control,
                 listen=a.listen, transport=a.transport, cache_pages=640,
                 load_timeout_ms=a.timeout_ms, metrics_file=str(metrics), cuda_batch=True,
                 save_prefix_tokens=8192, disable_save=a.mode not in ('prime', 'cpu-hot'),
                 rdma_pipeline=a.mode == 'optimized' or a.mode.startswith('fault-'), direct_receive=a.mode in ('optimized', 'direct') or a.mode.startswith('fault-'),
                 read_chunk_pages=a.chunk_pages)
    if a.mode == 'prime':
        extra['snapshot_export_dir'] = str(a.snapshot)
    options = ['--no-enable-prefix-caching']
    if a.mode != 'cold': options += kvfabric_config(extra)
    if a.profile: options += ['--profiler-config', '{"profiler":"cuda"}']
    env = dict(VLLM_BATCH_INVARIANT='1', VLLM_NVTX_SCOPES_FOR_PROFILING='1', KVF_PERF_TRACE='1',
               OMP_NUM_THREADS='2', VLLM_NO_USAGE_STATS='1')
    with server(a.model, a.out / 'vllm.log', 16400, 1 << 30, 1, options, env) as url:
        if a.mode == 'prime':
            result = generate(url, prefix, 2)
            wait_kvf_publication(metrics, result['request_id'], len(prefix))
            wait_kvf_idle(metrics)
            assert (a.snapshot/'kv.bin').stat().st_size == 288 << 20
            assert json.loads((a.snapshot/'tokens.json').read_text())[:8192] == prefix
            write(a.out/'prime.json', result)
            return
        # Heat model execution identically in each mode; no target suffix repeats.
        if a.mode == 'cpu-hot':
            primed = generate(url, prefix, 2)
            wait_kvf_publication(metrics, primed['request_id'], len(prefix))
            wait_kvf_idle(metrics)
        else:
            generate(url, [100 + i % 20000 for i in range(8192)], 2)
        for prompt in prompts[-2:]: generate(url, prompt, 16)
        reset_gpu_cache(url)
        before = token_sources(get(url+'/metrics').decode())
        if a.fault_gate:
            (a.fault_gate.with_suffix('.ready')).write_text('warm; ready for data-only netem\n')
            deadline=time.monotonic()+30
            while not a.fault_gate.with_suffix('.armed').exists():
                if time.monotonic()>deadline: raise RuntimeError('fault coordinator timeout')
                time.sleep(.05)
        if a.profile:
            with HTTP.open(urllib.request.Request(url+'/start_profile', method='POST'), timeout=30): pass
        if a.profile: os.environ['KVF_CLIENT_TRACE']='1'
        rows = []
        for i, prompt in enumerate(prompts[:a.rounds]):
            with nvtx.annotate('rdma.measured.'+str(i)):
                row = generate(url, prompt, 16)
            row.update(index=i, mode=a.mode)
            rows.append(row)
            with (a.out/'requests.jsonl').open('a') as f: f.write(json.dumps(row)+'\n')
        if a.profile:
            with HTTP.open(urllib.request.Request(url+'/stop_profile', method='POST'), timeout=30): pass
        measured_events=[e for e in read_events(metrics) if
            any(e.get('request_id','').startswith(r['request_id']+'-') for r in rows)]
        actual_hit_tokens=sum(e.get('tokens',0) for e in measured_events if e['event']=='hit')
        # Prometheus stats may lag the streaming response by a log interval.
        for _ in range(30):
            after = token_sources(get(url+'/metrics').decode())
            hits = {k:v-before.get(k,0) for k,v in after.items()}
            if sum(hits.values()) == 16384*a.rounds:break
            time.sleep(.2)
        expected = 0 if a.mode == 'cold' else actual_hit_tokens
        if not a.mode.startswith('fault-'):
            assert hits.get('external_kv_transfer', 0) == expected, hits
        else:
            events=read_events(metrics)
            failures=[e for e in events if e['event']=='load_failure' and
                      any(e['request_id'].startswith(r['request_id']+'-') for r in rows)]
            assert len(failures)==len(rows), failures
            # Metadata path is excluded from netem: known tickets must release
            # immediately. Unknown ACQUIRE replies elsewhere are TTL bounded.
            from test_direct_snapshot import Client,Stats
            client=Client(timeout_ms=1000)
            lib=C.CDLL(str(a.build/'src/integration/libkvf_vllm.so'))
            lib.kvf_client_stats.argtypes=[Client,C.POINTER(Stats)]
            lib.kvf_endpoint_parse.argtypes=[C.c_char_p,C.c_void_p]
            assert lib.kvf_endpoint_parse(a.control.encode(),C.byref(client.endpoint))==0
            stats=Stats();assert lib.kvf_client_stats(client,C.byref(stats))==0
            assert stats.leases==0, stats.leases
            write(a.out/'fault-verification.json',dict(failures=failures,active_leases=stats.leases,
                  fallback_tokens_match=True,kind=a.mode))
        if a.reference:
            reference = read_events(a.reference)
            assert [r['output_ids'] for r in rows] == [r['output_ids'] for r in reference[:a.rounds]]
            assert [r['prompt_sha256'] for r in rows] == [r['prompt_sha256'] for r in reference[:a.rounds]]
        write(a.out/'summary.json', dict(mode=a.mode, requests=a.rounds,
             ttft_p50_ms=statistics.median(r['ttft_ms'] for r in rows), token_sources=hits,
             load_failures=sum(e['event']=='load_failure' for e in measured_events),
             workload=dict(input_tokens=16384, prefix_tokens=8192, kv_bytes=288 << 20,
                           output_tokens=16, eager=True, batch_invariant=True),
             reference_verified=bool(a.reference), profile=a.profile, read_chunk_pages=a.chunk_pages))


def source(a):
    a.out.mkdir(parents=True, exist_ok=True)
    values = json.loads((a.snapshot/'spec.json').read_text())
    values['control'] = a.control
    spec = Spec(**{k:v.encode() if isinstance(v,str) else v for k,v in values.items()})
    b = Bridge(str(a.build/'src/integration/libkvf_vllm.so'), spec)
    ids, hashes = b.chain(json.loads((a.snapshot/'tokens.json').read_text())[:8192])
    stride = spec.layers*spec.layer_bytes
    raw = (a.snapshot/'kv.bin').read_bytes()
    assert len(raw) == 288 << 20
    data = C.create_string_buffer(raw, len(raw)); del raw
    stop = False
    def finish(*_):
        nonlocal stop
        stop = True
    signal.signal(signal.SIGTERM, finish)
    signal.signal(signal.SIGINT, finish)
    try:
        b.open('cross-instance-source', None, 640, 4, a.listen, a.transport, a.timeout_ms)
        for first in range(0,8192//16,b.window):
            pages = min(b.window,8192//16-first)
            assert b.transfer(ids,hashes,first,pages,C.addressof(data)+first*stride) == 0
        assert b.lookup(hashes,0,512,5000) == 512
        (a.out/'source.ready').write_text('288MiB published; strict '+a.transport+'\n')
        while not stop: time.sleep(.1)
    finally: b.close()


def databench(a):
    a.out.mkdir(parents=True,exist_ok=True)
    values=json.loads((a.snapshot/'spec.json').read_text());values['control']=a.control
    spec=Spec(**{k:v.encode() if isinstance(v,str) else v for k,v in values.items()})
    b=Bridge(str(a.build/'src/integration/libkvf_vllm.so'),spec)
    ids,hashes=b.chain(json.loads((a.snapshot/'tokens.json').read_text())[:8192])
    data=C.create_string_buffer(288 << 20)
    expected=hashlib.sha256((a.snapshot/'kv.bin').read_bytes()).hexdigest()
    stride=spec.layers*spec.layer_bytes
    results=[]
    try:
        b.open('data-tuning-sink',None,16,4,a.listen,'rdma',30000)
        for i in range(a.rounds+2):
            start=time.perf_counter_ns();status=0
            for first in range(0,512,a.chunk_pages):
                status=b.transfer(ids,hashes,first,min(a.chunk_pages,512-first),
                         C.addressof(data)+first*stride,30000,direct=True)
                if status:break
            elapsed=time.perf_counter_ns()-start
            verified=not status and hashlib.sha256(memoryview(data)).hexdigest()==expected
            assert verified,(i,status)
            if i>=2:results.append(dict(index=i-2,receive_ns=elapsed,verified=True,payload_bytes=288 << 20))
        write(a.out/'data-results.json',dict(rows=results,rounds=a.rounds,
                p50_ms=statistics.median(r['receive_ns'] for r in results)/1e6,
                environment={k:v for k,v in os.environ.items() if k.startswith('KVF_RDMA_')}))
    finally:b.close()


def orchestrate(a):
    """Root coordinator; model and data processes drop back to --user."""
    if os.geteuid() != 0: raise SystemExit('Use the shell namespace wrapper')
    commands = []
    uid=int(subprocess.check_output(["id","-u",a.user]));gid=int(subprocess.check_output(["id","-g",a.user]))
    path = str(Path(a.python).parent)+':/home/'+a.user+'/.local/bin:/usr/local/cuda/bin:/usr/sbin:/usr/bin:/sbin:/bin'
    def ns(n, cmd, env=()):
        env=[*env,*(['KVF_RDMA_BUFFER_BYTES='+str(a.buffer_bytes),'KVF_RDMA_SQSIZE='+str(a.queue_size),
            'KVF_RDMA_RQSIZE='+str(a.queue_size),'KVF_RDMA_INLINE='+str(a.inline),
            'KVF_RDMA_PEER_IDLE_MS=10000'] if a.buffer_bytes else [])]
        return ['ip','netns','exec','ns'+str(n),'runuser','-u',a.user,'--','env',
                'PATH='+path,'PYTHONPATH='+str(ROOT/'integrations/vllm')+':'+str(ROOT/'scripts'),
                'VLLM_BATCH_INVARIANT=1','KVF_PERF_TRACE=1',*env,*map(str,cmd)]
    def run(label,cmd,timeout=1200,fault=None):
        commands.append(dict(label=label,argv=cmd,start=time.time()))
        write(a.out/'commands.json',commands)
        with (a.out/(label+'.log')).open('w') as f, (a.out/(label+'-resources.jsonl')).open('w') as resources:
            child=subprocess.Popen(cmd,stdout=f,stderr=subprocess.STDOUT,start_new_session=True)
            limit=time.monotonic()+timeout
            initial_mem={k:int(v.split()[0]) for k,v in (line.split(':',1) for line in Path('/proc/meminfo').read_text().splitlines())}
            initial_swap=initial_mem['SwapTotal']-initial_mem['SwapFree']
            try:
                while child.poll() is None:
                    if fault and fault[0].with_suffix('.ready').exists() and not fault[0].with_suffix('.armed').exists():
                        netem=['delay','100ms'] if fault[1]=='delay' else ['loss','100%']
                        for tail in (["qdisc","replace","dev","veth1","root","handle","1:","prio"],
                                     ["qdisc","replace","dev","veth1","parent","1:3","handle","30:","netem",*netem],
                                     ["filter","add","dev","veth1","parent","1:","protocol","ip","prio","1","flower",
                                      "ip_proto","tcp","src_port","20001","flowid","1:3"]):
                            subprocess.run(['ip','netns','exec','ns1','tc',*tail],check=True)
                        fault[0].with_suffix('.armed').write_text('data-only '+fault[1]+'\n')
                    mem={k:int(v.split()[0]) for k,v in (line.split(':',1) for line in Path('/proc/meminfo').read_text().splitlines())}
                    resources.write(json.dumps(dict(time=time.time(),available_kib=mem['MemAvailable'],
                        swap_used_kib=mem['SwapTotal']-mem['SwapFree']))+'\n');resources.flush()
                    if time.monotonic()>limit or mem['MemAvailable'] < 1500*1024:
                        raise RuntimeError('Time/resource limit: '+label)
                    time.sleep(.5)
            finally:
                if child.poll() is None:
                    os.killpg(child.pid,signal.SIGTERM)
                    try: child.wait(timeout=10)
                    except subprocess.TimeoutExpired: os.killpg(child.pid,signal.SIGKILL);child.wait()
        commands[-1].update(returncode=child.returncode,end=time.time());write(a.out/'commands.json',commands)
        if child.returncode: raise RuntimeError('Failed '+label+'; inspect preserved log')
    def paired(label,server_cmd,client_cmd):
        with (a.out/(label+'-server.log')).open('w') as log:
            child=subprocess.Popen(server_cmd,stdout=log,stderr=subprocess.STDOUT)
            try:
                time.sleep(1)
                run(label,client_cmd,timeout=90)
                child.wait(timeout=10)
                assert child.returncode == 0
            finally:
                if child.poll() is None: child.terminate();child.wait(timeout=10)
    # Same direction as KV source->destination. Server in ns2, client in ns1.
    for repeat in range(0 if a.phase in ('workload','faults','tune','replay','final') else 3):
        for size in (16384, 4 << 20, 288 << 20):
            label=f'ceiling-r{repeat}-{size}'
            flags=['-R','-s',str(size),'-n','10' if size > 8<<20 else '1000','-t','8','-N','-F']
            paired(label,ns(2,['ib_write_bw','-d',a.provider+'2',*flags]),
                   ns(1,['ib_write_bw','-d',a.provider+'1',*flags,'10.10.0.2']))
        for size in (4 << 20,288 << 20):
            flags=['-S',str(size),'-I','3','-C','1','-B',str(4 << 20),'-T','v']
            paired(f'rstream-r{repeat}-{size}',ns(2,['rstream','-b','10.10.0.2',*flags]),
                   ns(1,['rstream','-s','10.10.0.2',*flags]))
    if a.phase == 'ceiling': return
    ctl='10.10.0.1:19000'
    config=a.out/'directory.conf';config.write_text('listen = '+ctl+'\nmax_pages = 4096\n')
    cmd=ns(1,[a.build/'bin/kvfabricd','--config',config])
    with ExitStack() as stack:
        log=stack.enter_context((a.out/'directory.log').open('w'))
        directory=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT)
        stack.callback(lambda: directory.terminate() if directory.poll() is None else None)
        time.sleep(1)
        common=[a.python,Path(__file__).resolve()]
        reference=a.reference or a.out/'cold/requests.jsonl'
        snap=a.out/'snapshot'
        if a.snapshot_from:
            old=a.snapshot_from
            assert (old/'kv.bin').stat().st_size == 288 << 20
            assert len(json.loads((old/'tokens.json').read_text())) == 8192
            if (old/'origin.json').exists():
                origin=json.loads((old/'origin.json').read_text())
                assert hashlib.sha256((old/'kv.bin').read_bytes()).hexdigest()==origin['sha256']
                original=Path(origin['source'])
            else: original=old
            events=read_events(original.parent/'prime/kvfabric.jsonl')
            coverage={(e['first'],e['pages']) for e in events if e['event']=='snapshot_exported'}
            assert coverage == {(0,128),(128,128),(256,128),(384,128)}, coverage
            snap.mkdir()
            for name in ('kv.bin','tokens.json','spec.json'): shutil.copy2(old/name,snap/name)
            write(snap/'origin.json',dict(source=str(old),gpu_exported_pages=512,
                sha256=hashlib.sha256((snap/'kv.bin').read_bytes()).hexdigest()))
        def worker_cmd(mode,out,rounds,profile=False):
            cmd=[*common,'worker','--mode',mode,'--out',out,'--model',a.model,'--build',a.build,
                 '--snapshot',snap,'--control',ctl,'--listen','10.10.0.2:0','--transport','rdma',
                 '--rounds',rounds,'--chunk-pages',a.chunk_pages]
            if mode=='prime': cmd[cmd.index('10.10.0.2:0')]='10.10.0.1:0'
            if mode not in ('cold','prime'): cmd+=['--reference',reference]
            if mode.startswith('fault-'):
                cmd+=['--timeout-ms','500','--fault-gate',out/'fault']
            if profile: cmd+=['--profile']
            return list(map(str,cmd))
        if a.phase not in ('faults','tune','replay','final') and not a.snapshot_from: run('prime',ns(1,worker_cmd('prime',a.out/'prime',1)))
        # Native recompute is run before the external source is started.
        if a.phase not in ('faults','tune','replay','final'):
            run('cold',ns(2,worker_cmd('cold',a.out/'cold',a.rounds)))
            run('cpu-hot',ns(2,worker_cmd('cpu-hot',a.out/'cpu-hot',a.rounds)))
        if a.phase == 'tune':
            for name,buffer,queue,inline in [('default',0,0,64),('buffer1m',1<<20,384,64),
                    ('buffer4m',4<<20,384,64),('queue1024',4<<20,1024,64),('buffer16m',16<<20,1024,0),('buffer1m-recheck',1<<20,384,64),('default-recheck',0,0,64)]:
                target=a.out/name;target.mkdir();os.chown(target,int(subprocess.check_output(['id','-u',a.user])),int(subprocess.check_output(['id','-g',a.user])))
                env=[] if not buffer else ['KVF_RDMA_BUFFER_BYTES='+str(buffer),'KVF_RDMA_SQSIZE='+str(queue),
                        'KVF_RDMA_RQSIZE='+str(queue),'KVF_RDMA_INLINE='+str(inline),'KVF_RDMA_PEER_IDLE_MS=10000']
                cmd=ns(1,[*common,'source','--out',target,'--build',a.build,'--snapshot',snap,
                          '--control',ctl,'--listen','10.10.0.1:20001','--transport','rdma'],env)
                with (target/'source.log').open('w') as source_log:
                    src=subprocess.Popen(cmd,stdout=source_log,stderr=subprocess.STDOUT)
                    try:
                        end=time.monotonic()+60
                        while not (target/'source.ready').exists():
                            if src.poll() is not None or time.monotonic()>end:raise RuntimeError('source startup: '+name)
                            time.sleep(.1)
                        try:
                            run(name,ns(2,[*common,'databench','--out',target,'--build',a.build,'--snapshot',snap,
                            '--control',ctl,'--listen','10.10.0.2:0','--rounds',a.rounds],env))
                        except RuntimeError as error:
                            write(target/'rejected.json',dict(error=str(error),accepted=False))
                    finally:
                        if src.poll() is None:src.terminate()
                        src.wait(timeout=20)
            directory.terminate();directory.wait(timeout=15)
            return
        source_cmd=ns(1,[*common,'source','--out',a.out,'--build',a.build,'--snapshot',snap,
                        '--control',ctl,'--listen','10.10.0.1:20001','--transport','rdma'])
        source_log=stack.enter_context((a.out/'source.log').open('w'))
        src=subprocess.Popen(source_cmd,stdout=source_log,stderr=subprocess.STDOUT)
        stack.callback(lambda: src.terminate() if src.poll() is None else None)
        end=time.monotonic()+90
        while not (a.out/'source.ready').exists():
            if src.poll() is not None or time.monotonic()>end: raise RuntimeError('source failed')
            time.sleep(.1)
        for mode in (() if a.phase == 'faults' else ('optimized',) if a.phase == 'final' else ('direct','optimized') if a.phase == 'replay' else ('baseline',) if a.phase == 'reference' else ('baseline','direct','optimized')):
            run(mode,ns(2,worker_cmd(mode,a.out/mode,a.rounds)))
        if a.phase == 'reference':
            src.terminate();src.wait(timeout=20)
            directory.terminate();directory.wait(timeout=15)
            return
        # Profiling samples are separate from unprofiled latency distributions.
        for mode in (() if a.phase == 'faults' else ('optimized',) if a.phase == 'final' else ('baseline','optimized')):
            target=a.out/('profile-'+mode);target.mkdir();os.chown(target,uid,gid)
            cmd=['nsys','profile','--trace=cuda,nvtx,osrt','--sample=none','--cpuctxsw=none',
                 '--capture-range=cudaProfilerApi','--capture-range-end=stop',
                 '--trace-fork-before-exec=true','-o',target/'trace',
                 *worker_cmd(mode,target,3,True)]
            run('profile-'+mode,ns(2,cmd))
            run('export-'+mode,ns(2,['nsys','export','--type=sqlite','--output',
                target/'trace.sqlite',target/'trace.nsys-rep']))
        for kind in ('delay','cut'):
            mode='fault-'+kind;target=a.out/mode;target.mkdir();os.chown(target,uid,gid)
            try:
                run(mode,ns(2,worker_cmd(mode,target,1)),fault=(target/'fault',kind))
            finally:
                with (target/'tc-qdisc.json').open('w') as f:
                    subprocess.run(['ip','netns','exec','ns1','tc','-s','-j','qdisc','show','dev','veth1'],stdout=f,check=False)
                subprocess.run(['ip','netns','exec','ns1','tc','qdisc','del','dev','veth1','root'],check=False)
        run('recovery',ns(2,worker_cmd('optimized',a.out/'recovery',1)))
        src.terminate();src.wait(timeout=20)
        directory.terminate();directory.wait(timeout=15)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('action',choices=('orchestrate','worker','source','databench'))
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--build',type=Path,default=ROOT/'build-cuda-release')
    p.add_argument('--model',default='')
    p.add_argument('--rounds',type=int,default=15)
    p.add_argument('--mode',choices=('prime','cold','cpu-hot','baseline','direct','optimized','fault-delay','fault-cut'),default='optimized')
    p.add_argument('--control',default='10.10.0.1:19000')
    p.add_argument('--listen',default='10.10.0.2:0')
    p.add_argument('--transport',choices=('rdma','tcp'),default='rdma')
    p.add_argument('--timeout-ms',type=int,default=30000)
    p.add_argument('--chunk-pages',type=int,default=32)
    p.add_argument('--snapshot',type=Path)
    p.add_argument('--snapshot-from',type=Path)
    p.add_argument('--reference',type=Path)
    p.add_argument('--profile',action='store_true')
    p.add_argument('--fault-gate',type=Path)
    p.add_argument('--buffer-bytes',type=int,default=0)
    p.add_argument('--queue-size',type=int,default=384)
    p.add_argument('--inline',type=int,default=64)
    p.add_argument('--python',default=sys.executable)
    p.add_argument('--user',default=os.environ.get('SUDO_USER','wty'))
    p.add_argument('--provider',choices=('rxe','siw'),default='rxe')
    p.add_argument('--phase',choices=('all','ceiling','workload','faults','tune','replay','final','reference'),default='all')
    a=p.parse_args()
    if a.rounds < 1 or not 1 <= a.chunk_pages <= 128: p.error('invalid rounds/chunk')
    a.out=a.out.resolve();a.build=a.build.resolve()
    # SIW's TCP listener shares the ephemeral port space with metadata sockets.
    # Reserve benchmark service ports outside ip_local_port_range.
    if a.listen == '10.10.0.2:0': a.listen='10.10.0.2:20002'
    elif a.listen == '10.10.0.1:0': a.listen='10.10.0.1:20003'
    globals()[a.action](a)

if __name__ == '__main__': main()
