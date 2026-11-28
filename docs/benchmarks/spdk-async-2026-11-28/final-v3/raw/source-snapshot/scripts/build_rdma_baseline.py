#!/usr/bin/env python3
"""Rebuild the observed serial bridge/CM baseline on the same final source tree."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('build',type=Path)
    a=p.parse_args()
    if a.source.exists() or a.build.exists():p.error('Use new source/build directories')
    a.source.mkdir(parents=True)
    for n in ('src','include','tools','tests','third_party','examples','cmake'):
        shutil.copytree(ROOT/n,a.source/n,symlinks=True)
    shutil.copy2(ROOT/'CMakeLists.txt',a.source/'CMakeLists.txt')
    for n,target in [('transfer.c','src/worker/remote/transfer.c'),
                     ('remote_sock.c','src/worker/remote/remote_sock.c'),
                     ('vllm_bridge.c','src/integration/vllm_bridge.c')]:
        shutil.copy2(ROOT/'docs/benchmarks/rdma-cross-instance-2026-11-26/baseline-source'/n,a.source/target)
    subprocess.run(['cmake','-S',str(a.source),'-B',str(a.build),'-DCMAKE_BUILD_TYPE=Release',
        '-DMINIINFER_WITH_CUDA=ON','-DMINIINFER_BUILD_VLLM_BRIDGE=ON','-DMINIINFER_VLLM_CUDA=ON',
        '-DMINIINFER_WITH_URING=ON','-DMINIINFER_WITH_RDMA=ON','-DCMAKE_CUDA_ARCHITECTURES=89'],check=True)
    subprocess.run(['cmake','--build',str(a.build),'-j2'],check=True)
    (a.source/'baseline-sha256.json').write_text(json.dumps({str(p.relative_to(a.source)):
        hashlib.sha256(p.read_bytes()).hexdigest()for p in a.source.rglob('*') if p.is_file()},indent=2))
if __name__=='__main__':main()
