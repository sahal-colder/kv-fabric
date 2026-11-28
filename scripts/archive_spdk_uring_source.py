#!/usr/bin/env python3
"""Archive the measured working-tree dependencies, excluding models/backups."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('directory',type=Path)
args=p.parse_args();out=args.directory.resolve()
files={Path('CMakeLists.txt'),Path('README.md')}
allowed={'.c','.h','.cu','.cuh','.cmake','.in','.py','.sh','.S'}
for directory in ['src','include','cmake','tools','tests','examples','third_party/infer-io']:
    for f in (root/directory).rglob('*'):
        if f.is_file() and (f.suffix in allowed or f.name in {'CMakeLists.txt','LICENSE','NOTICE'}):
            files.add(f.relative_to(root))
for name in ['run_spdk_uring_compare.py','summarize_spdk_uring_compare.py',
             'archive_spdk_uring_source.py','spdk_device.py','test_spdk_nvme.py']:
    files.add(Path('scripts')/name)
archive=out/'measured-source.tar.gz'
with tarfile.open(archive,'w:gz') as target:
    for f in sorted(files):target.add(root/f,arcname=str(f),recursive=False)
manifest={str(f):hashlib.sha256((root/f).read_bytes()).hexdigest() for f in sorted(files)}
for name in ['kvf_backend_compare','bin/miniinfer','bin/kvf_spdk_verify']:
    f=Path('build-backend-compare')/name
    manifest[str(f)]=hashlib.sha256((root/f).read_bytes()).hexdigest()
manifest[archive.name]=hashlib.sha256(archive.read_bytes()).hexdigest()
commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
(out/'source-sha256.json').write_text(json.dumps({
    'benchmark_commit':commit,'dirty_dependencies':True,'sha256':manifest},indent=2)+'\n')
print(f'{len(files)} source files; {archive.stat().st_size} archive bytes')
