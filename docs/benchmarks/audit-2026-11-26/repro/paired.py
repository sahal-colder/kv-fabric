import subprocess,pathlib,json,os
root=pathlib.Path('/home/wty/Infra');out=pathlib.Path('/tmp/kvf-audit');record=[]
for i in range(8):
 for label in (['before','after'] if i%2==0 else ['after','before']):
  name=f'paired-{label}-{i}';build='build-baseline' if label=='before' else 'build-release'
  subprocess.run(['python3','/tmp/kvf-audit/run-bench.py',build,name],cwd=root,check=True)
  record.append({'label':label,'round':i,'build':build,'log':'/tmp/kvf_bench_'+name+'.txt'})
for i in range(4):
 for label in (['before','after'] if i%2==0 else ['after','before']):
  name=f'paired-cuda-{label}-{i}';folder=out/name;folder.mkdir(exist_ok=True)
  build=out/'original-cuda' if label=='before' else root/'build-baseline-cuda'
  with (folder/'telemetry.csv').open('w') as f:
   subprocess.run(['nvidia-smi','--query-gpu=clocks.sm,power.draw,temperature.gpu,memory.used','--format=csv'],stdout=f,check=True)
  env=dict(os.environ,KVF_CUDA_RAW_CSV=str(folder/'dma-raw.csv'))
  with (folder/'cuda.csv').open('w') as f:subprocess.run([str(build/'bin/kvf_cuda_bench'),'all'],stdout=f,stderr=subprocess.STDOUT,env=env,check=True)
  with (folder/'recovery.csv').open('w') as f:subprocess.run([str(build/'bin/kvf_nvme_recovery_bench'),str(folder),'direct-uring-fixed'],stdout=f,stderr=subprocess.STDOUT,check=True)
  record.append({'label':label,'round':i,'build':str(build),'dir':str(folder)})
(out/'paired-commands.json').write_text(json.dumps(record,indent=2))
print('8 alternating CPU pairs and 4 alternating GPU pairs complete')
