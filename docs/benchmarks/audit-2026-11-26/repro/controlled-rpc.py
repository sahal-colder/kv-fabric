import pathlib,subprocess,select,json,re,statistics
root=pathlib.Path('/home/wty/Infra');out=pathlib.Path('/tmp/kvf-audit');result={}
for client_core,server_core in [(2,4),(12,13)]:
 for i in range(12):
  for label in (['before','after'] if i%2==0 else ['after','before']):
   build=root/('build-baseline' if label=='before' else 'build-release')
   p=subprocess.Popen(['taskset','-c',str(server_core),str(build/'bin/kvfabricd'),'--listen','127.0.0.1:0'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
   try:
    assert select.select([p.stdout],[],[],10)[0]
    line=p.stdout.readline();assert line.startswith('READY '),line
    micro=out/('micro-original' if label=='before' else 'micro-latest')
    text=subprocess.check_output(['taskset','-c',str(client_core),str(micro),line.split()[1]],text=True)
    (out/f'controlled-rpc-{client_core}-{label}-{i}.csv').write_text(text)
    value=statistics.median(float(v) for v in re.findall(r'^directory_rpc,\d+,([\d.]+),ns$',text,re.M))
    result.setdefault(str(client_core),{}).setdefault(label,[]).append(value)
   finally:
    p.terminate();p.wait(timeout=10)
(out/'controlled-rpc.json').write_text(json.dumps(result,indent=2))
print('P-core and E-core Directory/client fixed affinity: 12 pairs each')
