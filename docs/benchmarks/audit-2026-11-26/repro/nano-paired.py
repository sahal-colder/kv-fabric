from pathlib import Path
import subprocess,os
root=Path('/home/wty/Infra');out=Path('/tmp/kvf-audit')
for device in ['cpu','cuda']:
 for label in ['before','after']:
  build=(root/'build-baseline' if device=='cpu' else out/'original-cuda') if label=='before' else root/('build-release' if device=='cpu' else 'build-cuda-release')
  cmd=['python3',str(root/'tests/nanovllm/test_remote_inference.py'),'--directory',str(build/'bin/kvfabricd'),'--worker',str(build/'bin/test_nano_remote_worker'),'--device',device,'--benchmark',str(out/f'nano-{device}-{label}.csv'),'--samples','12','--warmup','1']
  with (out/f'nano-{device}-{label}.log').open('w') as f:
   print('COMMAND',*cmd,file=f,flush=True)
   subprocess.run(cmd,cwd=root,env=dict(os.environ,OPENBLAS_NUM_THREADS='1',OMP_NUM_THREADS='1'),stdout=f,stderr=subprocess.STDOUT,timeout=150,check=True)
print('deterministic synthetic Nano benchmark 12 rounds/device/build complete')
