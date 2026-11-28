import sys,ctypes as C,subprocess,pathlib,select,tempfile
root=pathlib.Path('/home/wty/Infra');sys.path.insert(0,str(root/'integrations/vllm'))
from kvfabric_vllm.bridge import Bridge,Spec
build=root/sys.argv[1];p=subprocess.Popen([str(build/'bin/kvfabricd'),'--listen','127.0.0.1:0'],stdout=subprocess.PIPE,text=True)
try:
 assert select.select([p.stdout],[],[],10)[0];endpoint=p.stdout.readline().split()[1]
 for layer_bytes in [128,4100]:
  spec=Spec(endpoint.encode(),b'nonaligned-budget',str(layer_bytes).encode(),4,3,layer_bytes)
  a,b=[Bridge(str(build/'src/integration/libkvf_vllm.so'),spec) for _ in range(2)]
  try:
   a.open('source',None,8,2);b.open('sink',None,8,2)
   tokens=list(range(32));ids,hashes=a.chain(tokens);size=8*3*layer_bytes
   payload=bytes(i%251 for i in range(size));src=C.create_string_buffer(payload,size);dst=C.create_string_buffer(size)
   assert a.transfer(ids,hashes,0,8,C.addressof(src))==0
   assert b.transfer(ids,hashes,0,8,C.addressof(dst),2000)==0
   assert dst.raw==payload
   print('bridge full 8-page roundtrip layer_bytes=',layer_bytes,'bytes=',size,'PASS')
  finally:b.close();a.close()
finally:p.terminate();p.wait(timeout=10)
