#!/usr/bin/env python3
"""Real snapshot ABI regression: many batches, no-match, deadline, no leaked leases.

TCP is used only as a portable correctness transport. This is not an RDMA/TCP
performance comparison and contributes no resume performance numbers.
"""
import argparse
import ctypes as C
import json
from pathlib import Path
import random
import time
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'scripts'),str(ROOT/'integrations/vllm')]
from bench_vllm_kv import directory
from kvfabric_vllm.bridge import Bridge, Spec

class Endpoint(C.Structure):
    _fields_=[('ipv4',C.c_ubyte*4),('port',C.c_uint16)]
class Client(C.Structure):
    _fields_=[('endpoint',Endpoint),('timeout_ms',C.c_uint32)]
class Stats(C.Structure):
    _fields_=[('pages',C.c_uint32),('workers',C.c_uint32),('leases',C.c_uint32),
              *[(n,C.c_uint64) for n in ('publishes','duplicates','lookups','matched_pages','invalidations')]]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('build',type=Path);p.add_argument('out',type=Path)
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
    rng=random.Random(805);records=[]
    with directory(a.build/'bin/kvfabricd',a.out/'directory.log') as ctl:
        spec=Spec(ctl.encode(),b'direct-regression',b'direct',16,3,4096)
        src,dst=[Bridge(str(a.build/'src/integration/libkvf_vllm.so'),spec) for _ in range(2)]
        src.open('direct-source',None,64,4);dst.open('direct-sink',None,16,4)
        lib=src.lib
        lib.kvf_endpoint_parse.argtypes=[C.c_char_p,C.POINTER(Endpoint)]
        lib.kvf_client_stats.argtypes=[Client,C.POINTER(Stats)]
        client=Client(timeout_ms=1000);assert lib.kvf_endpoint_parse(ctl.encode(),C.byref(client.endpoint))==0
        def no_leases(eventual=False):
            deadline=time.monotonic()+(16 if eventual else 0)
            while True:
                s=Stats();assert lib.kvf_client_stats(client,C.byref(s))==0
                if s.leases==0: return
                assert eventual and time.monotonic()<deadline, s.leases
                time.sleep(.05)
        ids,hashes=src.chain([rng.randrange(20000) for _ in range(32*16)])
        size=32*spec.layers*spec.layer_bytes
        payload=rng.randbytes(size);source=C.create_string_buffer(payload,size);dest=C.create_string_buffer(size)
        try:
            for first in range(0,32,8): assert src.transfer(ids,hashes,first,8,C.addressof(source)+first*size//32)==0
            for repeat in range(5):
                assert dst.transfer(ids,hashes,0,32,C.addressof(dest),5000,direct=True)==0
                assert dest.raw==payload;no_leases()
            records.append('32-page direct receive spans four frames; all bytes match; leases=0')
            missing_ids,missing_hashes=src.chain([999999]*16)
            assert dst.transfer(missing_ids,missing_hashes,0,1,C.addressof(dest),500,direct=True)!=0
            no_leases();records.append('no match returns safely without an underflow')
            assert dst.transfer(ids,hashes,0,32,C.addressof(dest),0,direct=True)!=0
            no_leases();records.append('expired deadline; leases=0')
            # The destination holds no published replica: every retry uses source data.
            for _ in range(2):
                status=dst.transfer(ids,hashes,0,32,C.addressof(dest),1,direct=True)
                no_leases(eventual=True)
                records.append('1ms request completed or discarded safely (unknown acquire replies expire by TTL): '+dst.describe(status))
            assert dst.transfer(ids,hashes,0,32,C.addressof(dest),5000,direct=True)==0
            assert dest.raw==payload;no_leases();records.append('success after deadlines; leases=0')
        finally: dst.close();src.close()
    result=dict(passed=True,transport='TCP correctness only',verified_bytes=size*6,cases=records)
    (a.out/'summary.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
if __name__=='__main__':main()
