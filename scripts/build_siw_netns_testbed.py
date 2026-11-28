#!/usr/bin/env python3
"""Build an opt-in, temporary SIW testbed module; does not install/load it.

Upstream Linux 6.8 creates SIW devices in init_net. This single ownership change
registers each new device in its netdev's namespace BEFORE any users exist.
It deliberately does not add a fake disassociate callback or bypass checks for
moving a live device. This is software iWARP, not RXE or a physical RoCE HCA.
Requires matching running-kernel headers, compiler, and Secure Boot disabled
(or a module signing setup). Loading an out-of-tree module taints the kernel;
unloading does not clear that diagnostic bit until the next reboot.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
URL='https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+archive/v6.8/drivers/infiniband/sw/siw.tar.gz'

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
    expected=json.loads((ROOT/'tools/siw-netns-upstream-sha256.json').read_text())
    raw=urllib.request.urlopen(URL,timeout=30).read()
    (a.out/'upstream.tar.gz').write_bytes(raw)
    with tarfile.open(fileobj=io.BytesIO(raw),mode='r:gz') as archive:
        for name,digest in expected.items():
            member=archive.getmember(name)
            if not member.isfile() or '/' in name: raise ValueError('unexpected archive entry')
            data=archive.extractfile(member).read()
            if hashlib.sha256(data).hexdigest()!=digest: raise ValueError('source hash mismatch: '+name)
            (a.out/name).write_bytes(data)
    source=a.out/'siw_main.c';s=source.read_text()
    anchor='struct ib_device *base_dev = &sdev->base_dev;'
    assert s.count(anchor)==1
    s=s.replace(anchor,anchor+'\n\t/* Testbed-only: assign owner netns before registration; no existing users. */\n\twrite_pnet(&base_dev->coredev.rdma_net, dev_net(sdev->netdev));',1)
    source.write_text(s)
    make=a.out/'Makefile';make.write_text(make.read_text().replace('obj-$(CONFIG_RDMA_SIW)','obj-m'))
    kernel=subprocess.check_output(['uname','-r'],text=True).strip()
    with (a.out/'build.log').open('w') as log:
        subprocess.run(['make','-C','/lib/modules/'+kernel+'/build','M='+str(a.out.resolve()),'-j2','modules'],
                       stdout=log,stderr=subprocess.STDOUT,check=True)
    result=dict(source=URL,kernel=kernel,patch='register new SIW device in netdev namespace',
                archive_sha256=hashlib.sha256(raw).hexdigest(),
                module_sha256=hashlib.sha256((a.out/'siw.ko').read_bytes()).hexdigest(),
                upstream_files=expected,transport='software iWARP; no physical HCA')
    (a.out/'manifest.json').write_text(json.dumps(result,indent=2)+'\n')
    print(a.out.resolve()/'siw.ko')
if __name__=='__main__': main()
