#!/usr/bin/env python3
"""Run protected physical layers sequentially. Optional poll failures stay visible."""
import argparse
import json
import os
import signal
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--skip-profiles', action='store_true')
    parser.add_argument('--build', type=Path, default=Path('build-spdk'))
    args = parser.parse_args()
    if os.geteuid():
        parser.error('root required for the protected hardware runner')
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runner = Path(__file__).resolve().with_name('run_spdk_async_ab.py')
    phases = [('iopoll', ['--smoke', '--iopoll'], False),
              ('sqpoll', ['--smoke', '--sqpoll'], False),
              ('raw', [], True),
              ('migration', ['--layer', 'migration'], True),
              ('inference', ['--layer', 'inference'], True)]
    if args.skip_profiles:
        phases = [phase for phase in phases if phase[2]]
    result = {'phases': []}
    running = None
    interrupted = False
    def stop(signum, frame):
        nonlocal interrupted
        if interrupted:
            return
        interrupted = True
        # Terminal INT/HUP already reaches the foreground child process group.
        # A direct TERM of this orchestrator must be forwarded once.
        if running is not None and signum == signal.SIGTERM:
            running.send_signal(signum)
        print('waiting for protected runner restoration before stopping', flush=True)
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, stop)
    try:
        for name, flags, required in phases:
            command = [sys.executable, str(runner), '--build', str(args.build.resolve()),
                       '--out', str(out/name)] + flags
            print('starting '+name, flush=True)
            running = subprocess.Popen(command)
            returncode = running.wait()
            running = None
            result['phases'].append({'name': name, 'argv': command, 'required': required,
                                     'returncode': returncode})
            (out/'suite.json').write_text(json.dumps(result, indent=2)+'\n')
            if interrupted:
                raise RuntimeError('suite interrupted after runner restoration')
            if required and returncode:
                raise RuntimeError(name+' failed; its runner has completed restoration')
    finally:
        owner = runner.parents[1].stat()
        subprocess.run(['chown', '-R', f'{owner.st_uid}:{owner.st_gid}', str(out)], check=True)


if __name__ == '__main__':
    main()
