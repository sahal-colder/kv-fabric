import subprocess,pathlib,sys,os,select
if len(sys.argv)!=3:
 raise SystemExit("usage: run-bench.py BUILD_DIR NEW_OUTPUT_DIR")
root=pathlib.Path(__file__).resolve().parents[4];build=(root/sys.argv[1]).resolve();out=pathlib.Path(sys.argv[2]).resolve();out.mkdir(parents=True,exist_ok=False)
log=open(out/'commands-output.txt','w',buffering=1)
def run(args):
 print('COMMAND',*map(str,args),file=log);subprocess.run(list(map(str,args)),cwd=root,stdout=log,stderr=log,check=True)
run(['cc','-std=c11','-O2','-UNDEBUG','-Iinclude',pathlib.Path(__file__).with_name('micro.c'),build/'src/worker/libminiinfer_storage.a',build/'src/control/libkvfabric.a','-lcrypto','-lpthread','-o',out/'micro'])
run([out/'micro'])
for mode in ['buffered','direct']:
 run([build/'bin/kvf_bench','ssd',mode,out,'65536','4',out/(mode+'-raw.csv'),out/(mode+'-summary.csv')])
(out/'fixture').mkdir(exist_ok=True);run([build/'bin/kvf_bench','fixture','65536',out/'fixture'])
procs=[]
try:
 def start(args,name):
  print('COMMAND',*map(str,args),file=log);err=open(out/(name+'.err'),'w');p=subprocess.Popen(list(map(str,args)),cwd=root,stdout=subprocess.PIPE,stderr=err,text=True);procs.append(p)
  if not select.select([p.stdout],[],[],15)[0]:raise RuntimeError('startup timeout '+name)
  line=p.stdout.readline();print(line.strip(),file=log)
  if not line.startswith('READY '):raise RuntimeError(name+' startup failed')
  return p,line.split()[1]
 d,endpoint=start([build/'bin/kvfabricd','--listen','127.0.0.1:0'],'directory')
 f=out/'fixture';w,_=start([build/'bin/miniinfer_worker','serve','audit-source',endpoint,'127.0.0.1:0','cpu',f/'namespace',f/'tokens',f/'tensors'],'worker')
 run([out/'micro',endpoint])
 run([build/'bin/kvf_bench','run','pooled','cpu',endpoint,'65536','4',d.pid,w.pid,out/'remote-raw.csv',out/'remote-summary.csv'])
finally:
 for p in reversed(procs):
  p.terminate()
  try:p.wait(timeout=15)
  except subprocess.TimeoutExpired:p.kill();p.wait()
 for file in out.glob('*summary.csv'):
  print(file.name,file=log);print(file.read_text(),file=log)
 log.close()
