#!/usr/bin/env python3
"""Explicit physical SPDK validation in an already backed-up scratch region.

This writes only the supplied region. The caller identifies the dedicated bdev
and creates the durable original backup before binding it to VFIO. Outputs are
local raw logs; publish only reviewed CSV/JSON measurements.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--bdev', required=True)
    parser.add_argument('--offset', type=int, required=True)
    parser.add_argument('--capacity', type=int, required=True)
    parser.add_argument('--backup', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--rounds', type=int, default=30)
    parser.add_argument('--warmup', type=int, default=3)
    parser.add_argument('--inference', action='store_true', help='also run the CUDA fixture A/B/A test')
    args = parser.parse_args()
    if args.offset < 0 or args.offset % 4096 or args.capacity % 4096:
        parser.error('offset and capacity must be nonnegative and 4096-byte aligned')
    if not 16*1024*1024 <= args.capacity <= 256*1024*1024:
        parser.error('capacity must be between 16 and 256 MiB')
    if not 1 <= args.rounds <= 1000 or not 0 <= args.warmup <= 100:
        parser.error('invalid round count')
    if not args.backup.is_file() or args.backup.stat().st_size != args.capacity:
        parser.error('backup must already exist and exactly match capacity')
    root = Path(__file__).resolve().parents[1]
    build = args.build.resolve()
    backup = args.backup.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    out = args.out.resolve()
    command = [str(build/'bin/kvf_spdk_verify'), str(args.config.resolve()), args.bdev,
               str(args.offset), str(args.capacity)]
    result = {'warmup_rounds': args.warmup, 'measured_rounds': args.rounds,
              'backup_sha256': hashlib.sha256(backup.read_bytes()).hexdigest(),
              'exit_codes': {}, 'tiered': {}}

    def execute(name, options):
        with (out/(name+'.log')).open('w') as target:
            proc = subprocess.run(command+options, cwd=root, stdout=target,
                                  stderr=subprocess.STDOUT, timeout=600)
        result['exit_codes'][name] = proc.returncode
        if proc.returncode:
            raise RuntimeError(name+' failed; inspect '+str(out/(name+'.log')))

    # raw refuses a stale/mismatched backup before writing. Do not run an
    # unconditional external restore if that precondition fails.
    execute('raw', ['raw', str(backup)])
    try:
        for mode in ('cpu', 'cuda'):
            execute(mode, [mode, '--rounds', str(args.rounds), '--warmup', str(args.warmup)])
            rows = [line.split(',') for line in (out/(mode+'.log')).read_text().splitlines()
                    if line.startswith(mode+'-spdk,')]
            if len(rows) != args.rounds or any(int(row[2]) != int(row[5]) for row in rows):
                raise RuntimeError('sample/verified-byte count mismatch')
            result['tiered'][mode] = {'rounds': len(rows), 'payload_bytes': int(rows[0][2]),
                'recovery_p50_ms': statistics.median(float(row[4]) for row in rows)}
        if args.inference:
            spec = importlib.util.spec_from_file_location('fixture', root/'tests/nanovllm/test_remote_inference.py')
            fixture = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(fixture)
            model, tokenizer = fixture.fixtures(out)
            base = [str(build/'bin/miniinfer'), str(model), '-z', str(tokenizer),
                    '--device', 'cuda', '--stdin', '--blocks', '8', '--block-size', '8',
                    '--batch', '1', '-n', '4', '-t', '0', '--ids', '--ignore-eos']
            prompts = 'abcdefghijklmnopabcdefghijklmnop\nZYXWVUTSRQPONMLKZYXWVUTSRQPONMLK\nabcdefghijklmnopabcdefghijklmnop\n'
            runs = {}
            for name, extra in [('baseline', []), ('spdk', ['--ssd-mode', 'spdk', '--ssd-slots', '32',
                    '--spdk-config', str(args.config.resolve()), '--spdk-bdev', args.bdev,
                    '--spdk-offset', str(args.offset), '--spdk-iova-mode', 'va',
                    '--spdk-base-virtaddr', '0x1000000000'])]:
                proc = subprocess.run(base+extra, cwd=root, input=prompts, text=True,
                                      capture_output=True, timeout=60)
                (out/(name+'-inference.stdout')).write_text(proc.stdout)
                (out/(name+'-inference.stderr')).write_text(proc.stderr)
                if proc.returncode:
                    raise RuntimeError(name+' inference failed')
                runs[name] = proc
            reads = [int(n) for n in re.findall(r'ssd_reads=(\d+)', runs['spdk'].stderr)]
            if runs['baseline'].stdout != runs['spdk'].stdout or not reads or not reads[-1]:
                raise RuntimeError('output Token mismatch or no observed SSD cache read')
            result['inference_token_ids_equal'] = True
            result['inference_ssd_reads'] = reads[-1]
    finally:
        try:
            execute('restore', ['restore', str(backup)])
        finally:
            (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
