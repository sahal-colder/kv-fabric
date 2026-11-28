#!/usr/bin/env python3
"""Build the exact pre-optimization bridge with the same phase instrumentation.

The archived bridge is the initial working-tree implementation, not git HEAD
(the audit began with uncommitted source). Models, reports and build trees are
never copied. Both output directories must be new.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("source",type=Path);p.add_argument("build",type=Path)
    a=p.parse_args()
    if a.source.exists() or a.build.exists():
        p.error("source and build must be new directories")
    a.source.mkdir(parents=True)
    for name in ("src","include","tools","tests","third_party","examples","cmake"):
        shutil.copytree(ROOT/name,a.source/name,symlinks=True)
    shutil.copy2(ROOT/"CMakeLists.txt",a.source/"CMakeLists.txt")
    baseline=ROOT/"docs/benchmarks/perf-attribution-2026-11-26/before/bridge-diagnostic.c"
    shutil.copy2(baseline,a.source/"src/integration/vllm_bridge.c")
    subprocess.run(["cmake","-S",str(a.source),"-B",str(a.build),"-DCMAKE_BUILD_TYPE=Release",
        "-DMINIINFER_WITH_CUDA=ON","-DMINIINFER_BUILD_VLLM_BRIDGE=ON","-DMINIINFER_VLLM_CUDA=ON",
        "-DMINIINFER_WITH_URING=ON","-DCMAKE_CUDA_ARCHITECTURES=89"],check=True)
    subprocess.run(["cmake","--build",str(a.build),"-j8"],check=True)
    files={str(f.relative_to(a.source)):hashlib.sha256(f.read_bytes()).hexdigest()
           for f in a.source.rglob("*") if f.is_file()}
    (a.source/"source-sha256.json").write_text(json.dumps(files,indent=2))


if __name__=="__main__":main()
