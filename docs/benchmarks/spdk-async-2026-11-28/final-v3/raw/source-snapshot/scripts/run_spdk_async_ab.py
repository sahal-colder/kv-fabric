#!/usr/bin/env python3
"""Production-engine physical A/B, with identity checks, backups and restoration.

Default: all 7 sizes x 7 QDs x 5 workloads, 3 warmups and 30 measured rounds,
2 alternating backend orders. Root is needed only for driver switching.
"""
import argparse
import csv
import hashlib
import importlib.util
import json
import mmap
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

import run_spdk_uring_compare as safety

SIZES = [4096, 16384, 65536, 262144, 524288, 1048576, 4194304]
DEPTHS = [1, 4, 8, 16, 32, 64, 128]
WORKLOADS = ['seq-read', 'seq-write', 'rand-read', 'rand-write', 'mixed']


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def prepare(device, region):
    page = bytes(((i*61) ^ (i >> 8)) & 255 for i in range(4096))
    digest = hashlib.sha256()
    fd = os.open(device, os.O_RDWR | os.O_DIRECT | os.O_EXCL)
    try:
        with mmap.mmap(-1, 4*1024*1024) as buf:
            buf[:] = page * (len(buf)//4096)
            for pos in range(0, region['length'], len(buf)):
                n = min(len(buf), region['length']-pos)
                view = memoryview(buf)[:n]
                try:
                    require(os.pwritev(fd, [view], region['offset']+pos) == n, 'short prepare write')
                    digest.update(view)
                finally:
                    view.release()
            os.fsync(fd)
    finally:
        os.close(fd)
    return digest.hexdigest()


def backup_extension(device, scratch, out):
    # 256 * (512 KiB + 4 KiB) needs 129 MiB. Extend by exactly one MiB,
    # only after a durable private backup, without altering the original manifest.
    length = 1048576
    offset = scratch['offset']+scratch['length']
    suffix = hashlib.sha256(str(out).encode()).hexdigest()[:16]
    backup = safety.LOCAL/f'async-extension-{suffix}.original.bin'
    fd = os.open(device, os.O_RDONLY | os.O_DIRECT | os.O_EXCL)
    try:
        with mmap.mmap(-1, length) as buf:
            require(os.preadv(fd, [buf], offset) == length, 'short extension backup read')
            digest = hashlib.sha256(buf).hexdigest()
            target = os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            try:
                written = 0
                while written < length:
                    n = os.write(target, buf[written:])
                    require(n > 0, 'short extension backup write')
                    written += n
                os.fsync(target)
            finally:
                os.close(target)
    finally:
        os.close(fd)
    directory = os.open(backup.parent, os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    require(hashlib.sha256(backup.read_bytes()).hexdigest() == digest, 'extension backup SHA mismatch')
    return {'name': 'scratch-extension', 'offset': offset, 'length': length,
            'backup': str(backup), 'sha256': digest}


def run_nonraw(args, backend, target, batch, scratch, out, meta, save):
    def execute(name, command, **kwargs):
        entry = {'name': name, 'argv': list(map(str, command)), 'started': safety.now()}
        meta['commands'].append(entry)
        save()
        with (out/(name+'.log')).open('w') as log:
            proc = subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT,
                                  timeout=300, **kwargs)
        entry.update(returncode=proc.returncode, finished=safety.now())
        save()
        require(proc.returncode == 0, name+' failed')
        print(name+': PASS', flush=True)
    if args.layer == 'migration':
        counts = [32] if args.smoke else [1, 32, 64, 128, 256]
        depths = [1, 16] if args.smoke else DEPTHS
        devices = ['cpu', 'gpu']
        configs = [(count, depth, device) for count in counts for depth in depths for device in devices]
        if batch % 2:
            configs.reverse()
        for count, depth, device in configs:
            # All 256 records fit after the separately backed-up 1-MiB extension.
            size = 524288
            name = f'b{batch}-{backend}-{device}-pages{count}-qd{depth}'
            execute(name, ['taskset', '-c', '4', args.build.resolve()/'bin/kvf_migration_bench',
                           backend, target, 'Samsungn1', scratch['offset'], scratch['length'],
                           size, count, depth, device, 30, 3, out/(name+'.csv')])
        return
    import re
    spec = importlib.util.spec_from_file_location('fixture', safety.ROOT/'tests/nanovllm/test_remote_inference.py')
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    fixtures = out/'fixture'
    fixtures.mkdir(exist_ok=True)
    model, tokenizer = fixture.fixtures(fixtures)
    meta['fixture_sha256'] = {str(p.name): hashlib.sha256(p.read_bytes()).hexdigest() for p in [model, tokenizer]}
    save()
    common = ['taskset', '-c', '4', args.build.resolve()/'bin/miniinfer', model, '-z', tokenizer,
              '--device', 'cuda', '--stdin', '--blocks', 8, '--block-size', 8, '--batch', 1,
              '-n', 4, '-t', 0, '--ids', '--ignore-eos', '--trace']
    extra = ['--ssd-slots', 32]
    if backend == 'spdk':
        extra += ['--ssd-mode', 'spdk', '--spdk-config', target, '--spdk-bdev', 'Samsungn1',
                  '--spdk-offset', scratch['offset'], '--spdk-reactor-mask', '0x4',
                  '--spdk-iova-mode', 'va', '--spdk-base-virtaddr', '0x1000000000']
    else:
        extra += ['--ssd-mode', 'raw-uring', '--ssd-device', target, '--ssd-offset', scratch['offset'], '--ssd-io-cpu', 2]
    prompts = 'abcdefghijklmnopabcdefghijklmnop\nabcdefghijklmnopabcdefghijklmnop\nZYXWVUTSRQPONMLKZYXWVUTSRQPONMLK\nabcdefghijklmnopabcdefghijklmnop\n'
    baseline = None
    csv_path = out/f'b{batch}-{backend}-inference.csv'
    with csv_path.open('w') as output:
        writer = csv.writer(output)
        writer.writerow(['layer', 'backend', 'batch', 'sample', 'request', 'cache_case', 'ttft_ns', 'ssd_reads_total', 'ssd_restore_wait_ns', 'token_ids_equal'])
        for sample in range(-(1 if args.smoke else 3), 1 if args.smoke else 30):
            name = f'b{batch}-{backend}-infer-{sample+3:02d}'
            command = list(map(str, common+extra))
            entry = {'name': name, 'argv': command, 'started': safety.now()}
            meta['commands'].append(entry)
            save()
            proc = subprocess.run(command, input=prompts, capture_output=True, text=True, timeout=120)
            (out/(name+'.stdout')).write_text(proc.stdout)
            (out/(name+'.stderr')).write_text(proc.stderr)
            require(proc.returncode == 0, name+' failed')
            # A no-SSD baseline is run for every backend/batch and compared exactly.
            if baseline is None:
                reference = subprocess.run(list(map(str, common)), input=prompts, capture_output=True, text=True, timeout=120)
                require(reference.returncode == 0, 'baseline inference failed')
                baseline = reference.stdout
                (out/f'b{batch}-{backend}-baseline.stdout').write_text(reference.stdout)
                (out/f'b{batch}-{backend}-baseline.stderr').write_text(reference.stderr)
            require(proc.stdout == baseline, 'inference Token mismatch')
            reads = [int(v) for v in re.findall(r'ssd_reads=(\d+)', proc.stderr)]
            require(reads and reads[-1] > 0, 'no observed SSD inference recovery')
            latencies = re.findall(r'request=(\d+) ttft_ns=(\d+)', proc.stderr)
            waits = [int(v) for v in re.findall(r'ssd_restore_wait_ns=(\d+)', proc.stderr)]
            require(len(latencies) == 4 and len(waits) == 4, 'expected four cold/hot/B/SSD requests')
            for index, (request, ttft) in enumerate(latencies):
                writer.writerow(['C-inference', backend, batch, sample, index,
                                 ['cold-A', 'ram-hot-A', 'cold-B', 'ssd-return-A'][index], ttft, reads[index],
                                 waits[index]-(waits[index-1] if index else 0), True])
            output.flush()
            entry.update(returncode=proc.returncode, finished=safety.now())
            save()
            print(name+': PASS', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=safety.ROOT/'build-spdk')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--batches', type=int, default=2)
    parser.add_argument('--iopoll', action='store_true')
    parser.add_argument('--sqpoll', action='store_true')
    parser.add_argument('--layer', choices=['raw', 'migration', 'inference'], default='raw')
    parser.add_argument('--smoke', action='store_true', help='limited coverage, still 3/30 rounds')
    args = parser.parse_args()
    require(os.geteuid() == 0, 'root required for protected driver switching')
    require(2 <= args.batches <= 5, 'at least two alternating backend orders required')
    bench = args.build.resolve()/'bin/kvf_async_bench'
    require(bench.is_file(), 'build kvf_async_bench first')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    manifest = json.loads((safety.LOCAL/'backup-manifest.json').read_text())
    require(manifest['serial'] == safety.SERIAL and manifest['pci'] == '0000:07:00.0', 'identity mismatch')
    scratch = next(r for r in manifest['regions'] if r['name'] == 'scratch')
    require((scratch['offset'], scratch['length']) == (1073741824, 134217728), 'unapproved region')
    huge_path = Path('/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages')
    huge_original = int(huge_path.read_text())
    poll_path = Path('/sys/module/nvme/parameters/poll_queues')
    poll_original = poll_path.read_text().strip() if poll_path.exists() else None
    initial = safety.state()
    active = 'ActiveState=active\n' in initial['service']
    meta = {'started': safety.now(), 'initial_state': initial, 'manifest': manifest,
            'production_engines': True, 'layer': args.layer, 'rounds': 1 if args.smoke and args.layer == 'inference' else 30,
            'warmup': 1 if args.smoke and args.layer == 'inference' else 3,
            'original_hugepages_2m': huge_original, 'original_nvme_poll_queues': poll_original,
            'requested_iopoll': args.iopoll, 'requested_sqpoll': args.sqpoll, 'smoke': args.smoke, 'commands': [], 'checks': {}, 'durability': 'non-durable completion',
            'cpu_cycles_per_io': None, 'cpu_cycles_reason': 'per-thread PMU instrumentation; unavailable samples explicitly marked cycles_measured=0',
            'cpu_affinity': {'submitter': 4, 'io_owner': 2}, 'numa_policy': 'host default, recorded by lscpu'}
    verified = False
    restore_regions = [scratch]

    def save():
        (out/'run.json').write_text(json.dumps(meta, indent=2)+'\n')

    def interrupted(signum, frame):
        # Once cleanup starts, a second terminal signal must not interrupt restore.
        for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            signal.signal(sig, signal.SIG_IGN)
        raise RuntimeError('interrupted signal '+str(signum))

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    try:
        if args.iopoll:
            require(poll_original is not None, 'kernel NVMe poll queue parameter unavailable')
            if int(poll_original) < 1:
                poll_path.write_text('1')
        safety.command(['systemctl', 'stop', safety.SERVICE])
        device = safety.kernel_disk()
        spec = importlib.util.spec_from_file_location('device', safety.ROOT/'scripts/spdk_device.py')
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        helper.unused(Path('/sys/class/block')/Path(device).name)
        require(int((Path('/sys/class/block')/Path(device).name/'size').read_text())*512 == manifest['device_bytes'], 'capacity mismatch')
        meta['checks']['before'] = []
        for region in manifest['regions']:
            backup = Path(region['backup'])
            require(backup.stat().st_size == region['length'], 'backup size mismatch')
            digest = hashlib.sha256(backup.read_bytes()).hexdigest()
            current = safety.direct_hash(device, region)
            require(digest == current == region['sha256'], region['name']+' backup/original mismatch')
            meta['checks']['before'].append({'name': region['name'], 'sha256': current})
        verified = True
        save()
        if args.layer == 'migration' and not args.smoke:
            extension = backup_extension(device, scratch, out)
            require(safety.direct_hash(device, extension) == extension['sha256'], 'extension changed after backup')
            restore_regions.append(extension)
            manifest['regions'].append(extension)
            meta['checks']['before'].append({'name': extension['name'], 'sha256': extension['sha256']})
            scratch = {**scratch, 'length': scratch['length']+extension['length']}
            meta['migration_region'] = {'offset': scratch['offset'], 'length': scratch['length']}
            save()
        if args.layer == 'raw':
            largest = 524288 if args.smoke else 4194304
            required_pages = (256*1024*1024+128*largest+2097151)//2097152
            meta['required_hugepages_2m'] = required_pages
            if int(huge_path.read_text()) < required_pages:
                huge_path.write_text(str(required_pages))
                if int(huge_path.read_text()) < required_pages:
                    # Reclaim contiguous free extents; no permanent VM setting changes.
                    Path('/proc/sys/vm/compact_memory').write_text('1')
                    huge_path.write_text(str(required_pages))
            meta['allocated_hugepages_2m'] = int(huge_path.read_text())
            save()
            require(meta['allocated_hugepages_2m'] >= required_pages,
                    'insufficient contiguous hugepages for configured DMA pool plus runtime')
        runtime_config = json.loads((safety.ROOT/'config/spdk-samsung.json').read_text())
        attachments = [entry['params'] for subsystem in runtime_config['subsystems']
                       for entry in subsystem['config'] if entry['method'] == 'bdev_nvme_attach_controller']
        require(len(attachments) == 1 and attachments[0].get('traddr') == manifest['pci']
                and attachments[0].get('name') == 'Samsung' and attachments[0].get('trtype') == 'PCIe',
                'SPDK JSON does not match the verified physical controller')
        (out/'spdk-config.json').write_text(json.dumps(runtime_config, indent=2)+'\n')
        environment = {}
        for key, command in [('uname', ['uname', '-a']), ('cpu', ['lscpu']), ('gpu', ['nvidia-smi', '-q']),
                             ('disks', ['lsblk', '-o', 'NAME,MODEL,SERIAL,SIZE,TYPE,MOUNTPOINTS']),
                             ('nvme', ['nvme', 'id-ctrl', device]), ('smart_before', ['nvme', 'smart-log', device])]:
            try:
                proc = subprocess.run(command, text=True, capture_output=True)
                environment[key] = {'returncode': proc.returncode, 'stdout': proc.stdout, 'stderr': proc.stderr}
            except OSError as error:
                environment[key] = {'unavailable': str(error)}
        environment['governors'] = {str(p): p.read_text().strip() for p in Path('/sys/devices/system/cpu').glob('cpu[0-9]*/cpufreq/scaling_governor')}
        environment['spdk_config'] = (out/'spdk-config.json').read_text()
        environment['hugepages_2m'] = int(huge_path.read_text())
        (out/'environment.json').write_text(json.dumps(environment, indent=2)+'\n')
        sources = [p for prefix in ['src/worker', 'include/miniinfer', 'tools', 'scripts']
                   for p in (safety.ROOT/prefix).glob('*') if p.is_file() and p.suffix in {'.c', '.h', '.cu', '.py'}]
        meta['source_sha256'] = {str(p.relative_to(safety.ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
        meta['executable_sha256'] = {name: hashlib.sha256((args.build.resolve()/'bin'/name).read_bytes()).hexdigest()
            for name in ['kvf_async_bench', 'kvf_migration_bench', 'miniinfer']}
        dataset_hash = prepare(device, scratch) if args.layer == 'raw' else None
        if dataset_hash:
            require(safety.direct_hash(device, scratch) == dataset_hash, 'prepared dataset mismatch')
            meta['dataset_sha256'] = dataset_hash
        sizes = [4096, 524288] if args.smoke else SIZES
        depths = [1, 32, 128] if args.smoke else DEPTHS
        workloads = ['rand-read', 'mixed'] if args.smoke else WORKLOADS
        for batch in range(args.batches):
            order = ['uring', 'spdk'] if batch % 2 == 0 else ['spdk', 'uring']
            for backend in order:
                safety.command(['systemctl', 'start' if backend == 'spdk' else 'stop', safety.SERVICE])
                target = str(out/'spdk-config.json') if backend == 'spdk' else safety.kernel_disk()
                if args.layer != 'raw':
                    run_nonraw(args, backend, target, batch, scratch, out, meta, save)
                    continue
                configs = [(size, depth, workload) for size in sizes for depth in depths for workload in workloads]
                if batch % 2:
                    configs.reverse()
                for size, depth, workload in configs:
                    name = f'b{batch}-{backend}-{size}-qd{depth}-{workload}'
                    command = ['taskset', '-c', '4', str(bench), 'raw', backend, target, 'Samsungn1',
                               str(scratch['offset']), str(scratch['length']), str(size), str(depth), workload,
                               '30', '3', str(out/name), 'write-authorized']
                    if backend == 'uring':
                        if args.iopoll: command.append('iopoll')
                        if args.sqpoll: command.append('sqpoll')
                    entry = {'name': name, 'argv': command, 'started': safety.now()}
                    meta['commands'].append(entry)
                    save()
                    with (out/(name+'.log')).open('w') as log:
                        proc = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=300)
                    entry.update(returncode=proc.returncode, finished=safety.now())
                    save()
                    require(proc.returncode == 0, name+' failed')
                    rows = list(csv.DictReader((out/(name+'-rounds.csv')).open()))
                    require(len([r for r in rows if int(r['sample']) >= 0]) == 30, 'sample count mismatch')
                    if backend == 'uring':
                        require(all(bool(int(r['iopoll'])) == args.iopoll and bool(int(r['sqpoll'])) == args.sqpoll for r in rows), 'poll flags not actually enabled')
                    require(all(int(r['submitted']) == int(r['completed']) and int(r['max_inflight']) <= depth for r in rows), 'I/O accounting mismatch')
                    print(name+': PASS', flush=True)
                # Every workload writes the same page pattern. Check the full raw region outside timings.
                safety.command(['systemctl', 'stop', safety.SERVICE])
                device = safety.kernel_disk()
                require(safety.direct_hash(device, scratch) == dataset_hash, backend+' dataset integrity failed')
    except BaseException as error:
        meta['error'] = repr(error)
        raise
    finally:
        try:
            if verified:
                safety.command(['systemctl', 'stop', safety.SERVICE])
                device = safety.kernel_disk()
                helper.unused(Path('/sys/class/block')/Path(device).name)
                for region in restore_regions:
                    safety.restore(device, region)
                meta['checks']['after'] = []
                for region in manifest['regions']:
                    digest = safety.direct_hash(device, region)
                    meta['checks']['after'].append({'name': region['name'], 'sha256': digest, 'matches_original': digest == region['sha256']})
                    require(digest == region['sha256'], 'RESTORE FAILED: '+region['name'])
        finally:
            if poll_original is not None and poll_path.read_text().strip() != poll_original:
                poll_path.write_text(poll_original)
            meta['final_nvme_poll_queues'] = poll_path.read_text().strip() if poll_path.exists() else None
            if int(huge_path.read_text()) != huge_original:
                huge_path.write_text(str(huge_original))
            meta['final_hugepages_2m'] = int(huge_path.read_text())
            safety.command(['systemctl', 'start' if active else 'stop', safety.SERVICE])
            meta['final_state'] = safety.state()
            meta['finished'] = safety.now()
            save()
            safety.command(['chown', '-R', 'wty:wty', out])
            print('original region bytes/hashes and service state restored', flush=True)
    if args.layer == 'raw':
        subprocess.run([sys.executable, str(safety.ROOT/'scripts/summarize_spdk_async.py'), str(out)], check=True)
    else:
        subprocess.run([sys.executable, str(safety.ROOT/'scripts/summarize_spdk_migration.py'), args.layer, str(out)], check=True)


if __name__ == '__main__':
    main()
