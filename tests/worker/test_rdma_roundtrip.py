#!/usr/bin/env python3
"""Opt-in strict rsockets round trips on a preconfigured RDMA IPv4 interface.

No privileges or network configuration are performed by this test. It uses
private temporary fixtures, checks the observed transport, and compares every
payload byte. A software RXE device is valid functional coverage, not HCA speed.
"""
import argparse
import json
from pathlib import Path
import select
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--address', required=True)
    parser.add_argument('--devices', nargs='+', choices=['cpu','cuda'], default=['cpu','cuda'])
    parser.add_argument('--rounds', type=int, default=30)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error('--rounds must be positive')
    args.build = args.build.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    records = []
    processes = []

    def start(command, name):
        with (args.out/(name+'.stderr')).open('w') as errors:
            child = subprocess.Popen(list(map(str,command)),stdout=subprocess.PIPE,stderr=errors,text=True)
        processes.append(child)
        if not select.select([child.stdout],[],[],15)[0]:
            raise RuntimeError(name+' startup timeout')
        ready = child.stdout.readline()
        if not ready.startswith('READY '):
            raise RuntimeError(name+' failed startup; inspect stderr')
        return child,ready.split()[1]

    with tempfile.TemporaryDirectory(prefix='kvfabric-rdma-') as temp:
        fixture = Path(temp)
        subprocess.run([str(args.build/'bin/kvf_bench'),'fixture','65536',str(fixture)],check=True)
        payload = (fixture/'tensors').read_bytes()
        try:
            _,control = start([args.build/'bin/kvfabricd','--listen','127.0.0.1:0'],'directory')
            for device in args.devices:
                source,_ = start([args.build/'bin/miniinfer_worker','serve','source-'+device,
                    control,args.address+':0',device,fixture/'namespace',fixture/'tokens',fixture/'tensors',
                    '--kv-transport','rdma','--no-kv-rdma-fallback'],'source-'+device)
                for repeat in range(args.rounds):
                    destination = fixture/'received'
                    command = [args.build/'bin/miniinfer_worker','pull','sink-'+device+'-'+str(repeat),
                        control,args.address+':0',device,fixture/'namespace',fixture/'tokens',destination,
                        '--kv-transport','rdma','--no-kv-rdma-fallback']
                    result = subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=30)
                    (args.out/(device+'-'+str(repeat)+'.log')).write_text(result.stdout+result.stderr)
                    if result.returncode or 'active_transport: RDMA_RSOCKET' not in result.stderr:
                        raise AssertionError('Strict RDMA transfer failed or used an unexpected backend')
                    if destination.read_bytes() != payload:
                        raise AssertionError('RDMA payload mismatch')
                    records.append({'device':device,'round':repeat,'transport':'RDMA_RSOCKET',
                                    'verified_bytes':len(payload),'matched':True})
                source.terminate()
                source.wait(timeout=10)
        finally:
            for child in reversed(processes):
                if child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()
    (args.out/'summary.json').write_text(json.dumps({'passed':True,'roundtrips':len(records),
                                                  'records':records},indent=2))
    print('Strict RDMA round trips:',len(records),'PASS')


if __name__ == '__main__':
    main()
