#!/usr/bin/env python3
"""Explicit physical migration trace; production code plus NVTX-only link wrappers.

Requires the existing dedicated Samsung backup manifest, never a default CTest.
CUDA device copies are traced by Nsight; NVTX spans outstanding production SSD
calls including dispatch, completion and copies, not pure NVMe device service.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import signal
import sys

import run_spdk_uring_compare as safety


def require(value, message):
    if not value:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=safety.ROOT/'build-spdk')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    require(os.geteuid() == 0, 'root required for protected device switch')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    build = args.build.resolve()
    object_path = out/'trace_wrappers.o'
    binary = out/'kvf_migration_trace'
    compile_cmd = ['cc', '-std=c11', '-O2', '-DMINIINFER_WITH_URING=1',
                   '-I'+str(safety.ROOT/'include'), '-I/usr/local/cuda/include',
                   '-c', str(safety.ROOT/'tools/kvf_migration_trace.c'), '-o', str(object_path)]
    subprocess.run(compile_cmd, check=True)
    link = shlex.split((build/'src/worker/CMakeFiles/kvf_migration_bench.dir/link.txt').read_text())
    link[link.index('-o')+1] = str(binary)
    link += [str(object_path), '-Wl,--wrap=mi_spdk_transfer', '-Wl,--wrap=mi_uring_transfer', '-Wl,--wrap=mi_ssd_raw_uring_backend_create']
    subprocess.run(link, cwd=build/'src/worker', check=True)
    initial = safety.state()
    active = 'ActiveState=active\n' in initial['service']
    manifest = json.loads((safety.LOCAL/'backup-manifest.json').read_text())
    require(manifest['serial'] == safety.SERIAL and manifest['pci'] == '0000:07:00.0', 'device mismatch')
    scratch = next(r for r in manifest['regions'] if r['name'] == 'scratch')
    require((scratch['offset'], scratch['length']) == (1073741824, 134217728), 'region mismatch')
    meta = {'started': safety.now(), 'initial_state': initial, 'manifest': manifest,
            'commands': [compile_cmd, link], 'checks': {}, 'results': {},
            'scope': '128 x 512 KiB GPU Pages, concurrency16, 3 warmups + 3 trace rounds; timing not included in uninstrumented benchmark',
            'source_sha256': {str(p.relative_to(safety.ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in [safety.ROOT/'tools/kvf_migration_trace.c', safety.ROOT/'tools/kvf_migration_bench.c']},
            'production_library_sha256': hashlib.sha256((build/'src/worker/libminiinfer_storage.a').read_bytes()).hexdigest()}
    verified = False

    def save():
        (out/'run.json').write_text(json.dumps(meta, indent=2)+'\n')

    def interrupted(signum, frame):
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(sig, signal.SIG_IGN)
        raise RuntimeError('interrupted '+str(signum))

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, interrupted)
    try:
        safety.command(['systemctl', 'stop', safety.SERVICE])
        device = safety.kernel_disk()
        spec = importlib.util.spec_from_file_location('device', safety.ROOT/'scripts/spdk_device.py')
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        helper.unused(Path('/sys/class/block')/Path(device).name)
        meta['checks']['before'] = []
        for region in manifest['regions']:
            require(hashlib.sha256(Path(region['backup']).read_bytes()).hexdigest() == region['sha256'], 'backup SHA mismatch')
            digest = safety.direct_hash(device, region)
            require(digest == region['sha256'], 'original mismatch')
            meta['checks']['before'].append({'name': region['name'], 'sha256': digest})
        verified = True
        save()
        config = json.loads((safety.ROOT/'config/spdk-samsung.json').read_text())
        attachments = [entry['params'] for subsystem in config['subsystems'] for entry in subsystem['config']
                       if entry['method'] == 'bdev_nvme_attach_controller']
        require(len(attachments) == 1 and attachments[0].get('traddr') == manifest['pci'], 'SPDK controller mismatch')
        (out/'spdk-config.json').write_text(json.dumps(config, indent=2)+'\n')
        for backend in ['uring', 'spdk']:
            safety.command(['systemctl', 'start' if backend == 'spdk' else 'stop', safety.SERVICE])
            target = str(out/'spdk-config.json') if backend == 'spdk' else safety.kernel_disk()
            trace = out/(backend+'-trace')
            cmd = ['nsys', 'profile', '--trace=cuda,nvtx', '--sample=none', '--cpuctxsw=none',
                   '--force-overwrite=true', '-o', str(trace), 'taskset', '-c', '4', str(binary),
                   backend, target, 'Samsungn1', str(scratch['offset']), str(scratch['length']),
                   '524288', '128', '16', 'gpu', '3', '3', str(out/(backend+'.csv'))]
            meta['commands'].append(cmd)
            save()
            # Nsight honors SUDO_USER by dropping its target back to that user.
            # The protected raw node stays root-only; keep the profiler target
            # in the already-authorized root context, without changing node ACLs.
            env = dict(os.environ)
            for key in ('SUDO_USER', 'SUDO_UID', 'SUDO_GID', 'SUDO_COMMAND'):
                env.pop(key, None)
            with (out/(backend+'.log')).open('w') as log:
                proc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=180, env=env)
            meta['results'][backend] = proc.returncode
            save()
            require(proc.returncode == 0, backend+' trace failed')
            subprocess.run(['nsys', 'export', '--type=sqlite', '--force-overwrite=true',
                            '--output', str(out/(backend+'.sqlite')), str(trace)+'.nsys-rep'], check=True)
    except BaseException as error:
        meta['error'] = repr(error)
        raise
    finally:
        try:
            if verified:
                safety.command(['systemctl', 'stop', safety.SERVICE])
                device = safety.kernel_disk()
                helper.unused(Path('/sys/class/block')/Path(device).name)
                safety.restore(device, scratch)
                meta['checks']['after'] = []
                for region in manifest['regions']:
                    digest = safety.direct_hash(device, region)
                    require(digest == region['sha256'], 'RESTORE FAILED: '+region['name'])
                    meta['checks']['after'].append({'name': region['name'], 'sha256': digest, 'matches_original': True})
        finally:
            safety.command(['systemctl', 'start' if active else 'stop', safety.SERVICE])
            meta['final_state'] = safety.state()
            meta['finished'] = safety.now()
            save()
            safety.command(['chown', '-R', 'wty:wty', out])
            print('trace complete; original data and service restored', flush=True)


if __name__ == '__main__':
    main()
