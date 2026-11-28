import pathlib,subprocess
root=pathlib.Path('/home/wty/Infra');out=pathlib.Path('/tmp/kvf-audit')
for i in range(6):
 for label in (['before','after'] if i%2==0 else ['after','before']):
  build=out/'original-cuda' if label=='before' else root/'build-baseline-cuda'
  folder=out/f'final-cuda-{label}-{i}';folder.mkdir(exist_ok=True)
  with (folder/'recovery.csv').open('w') as f:subprocess.run([str(build/'bin/kvf_nvme_recovery_bench'),str(folder),'direct-uring-fixed'],stdout=f,stderr=subprocess.STDOUT,check=True)
print('6 alternating recovery pairs complete')
