from pathlib import Path
import re,csv,json,statistics as st
root=Path('/tmp/kvf-audit');samples={}
def add(label,key,value):samples.setdefault(key,{}).setdefault(label,[]).append(value)
for label in ['before','after']:
 for i in range(8):
  text=Path(f'/tmp/kvf_bench_final-owned-paired-{label}-{i}.txt').read_text()
  for key in ['tlsf_pair','page_pair','directory_rpc']:
   add(label,key,st.median(float(v) for v in re.findall('^'+key+r',\d+,([\d.]+),ns$',text,re.M)))
  for mode in ['buffered','direct','remote']:
   data=list(csv.reader((root/f'final-owned-paired-{label}-{i}'/(mode+'-summary.csv')).open()))
   for index,name in [(7,'mib_s'),(8,'p50_us'),(9,'p95_us'),(10,'p99_us')]:add(label,mode+'_'+name,st.median(float(r[index]) for r in data))
 for i in range(4):
  folder=root/f'final-owned-paired-cuda-{label}-{i}'
  for row in csv.reader((folder/'cuda.csv').open()):
   if len(row)==6:add(label,'cuda_'+row[0]+'_'+row[1]+'_ms',float(row[4]))
  data=list(csv.DictReader(line for line in (folder/'recovery.csv').read_text().splitlines() if line.startswith(('mode,','direct-'))))
  for name in ['gpu_to_host_ms','host_to_ssd_ms','ssd_to_gpu_ms','effective_recovery_gib_s']:add(label,'recovery_'+name,st.median(float(r[name]) for r in data))
result={}
for key,v in samples.items():
 a,b=st.median(v['before']),st.median(v['after']);delta=(b/a-1)*100
 ratios=[(y/x-1)*100 for x,y in zip(v['before'],v['after'])]
 result[key]={'before':a,'after':b,'change_percent':delta,'paired_change_median':st.median(ratios),'before_samples':v['before'],'after_samples':v['after']}
 print(f'{key:36} {a:12.3f} {b:12.3f} {delta:+8.2f}% paired {st.median(ratios):+7.2f}%')
(root/'performance-final-owned-paired.json').write_text(json.dumps(result,indent=2))
