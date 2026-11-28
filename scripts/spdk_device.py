#!/usr/bin/env python3
"""Bind one explicitly identified, unused NVMe to VFIO; never format a disk.

Install a root-owned copy for systemd. State and generated JSON are root-owned;
the device is selected by serial on every fresh boot, rather than nvme numbering.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import pwd
import subprocess
import time


def run(*args, check=True):
    return subprocess.run(args, check=check, capture_output=True, text=True)


def wait_for(predicate):
    for _ in range(100):
        value = predicate()
        if value:
            return value
        time.sleep(0.05)
    raise RuntimeError('device did not reach the requested state')


def driver(pci):
    link = pci / 'driver'
    return link.resolve().name if link.exists() else None


def bind(pci, name):
    run('modprobe', name)
    old = driver(pci)
    if old == name:
        return
    (pci / 'driver_override').write_text(name)
    try:
        if old:
            (pci / 'driver/unbind').write_text(pci.name)
        Path('/sys/bus/pci/drivers_probe').write_text(pci.name)
        wait_for(lambda: driver(pci) == name)
    finally:
        (pci / 'driver_override').write_text('\n')


def kernel_device(serial):
    for entry in Path('/sys/class/nvme').glob('nvme*'):
        if (entry / 'serial').read_text().strip() == serial:
            disks = list(entry.glob(entry.name + 'n*'))
            disks = [p for p in disks if (p / 'dev').is_file()]
            if len(disks) != 1:
                raise RuntimeError('expected exactly one namespace')
            return entry, disks[0]
    return None


def unused(disk):
    device = '/dev/' + disk.name
    info = json.loads(run('lsblk', '-J', '-o', 'PATH,MOUNTPOINTS', device).stdout)
    swaps = {line.split()[0] for line in Path('/proc/swaps').read_text().splitlines()[1:]}

    def inspect(node):
        path = node['path']
        if any(node.get('mountpoints') or []) or path in swaps:
            raise RuntimeError('target or a child is mounted/in swap: ' + path)
        if run('fuser', path, check=False).returncode == 0:
            raise RuntimeError('target is open: ' + path)
        for child in node.get('children', []):
            inspect(child)

    for node in info['blockdevices']:
        inspect(node)
    if list((disk / 'holders').iterdir()):
        raise RuntimeError('target is held by another block device')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['start', 'stop', 'status'])
    parser.add_argument('--serial', required=True)
    parser.add_argument('--user', required=True)
    args = parser.parse_args()
    if os.geteuid():
        parser.error('run the installed copy as root')
    account = pwd.getpwnam(args.user)
    directory = Path('/var/lib/kvfabric-spdk')
    directory.mkdir(mode=0o755, exist_ok=True)
    lock = open('/run/lock/kvfabric-spdk.lock', 'w')
    fcntl.flock(lock, fcntl.LOCK_EX)
    state_file = directory / 'device.json'
    state = json.loads(state_file.read_text()) if state_file.exists() else None
    boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    found = kernel_device(args.serial)
    if found:
        entry, disk = found
        pci = (entry / 'device').resolve()
        current = {'serial': args.serial, 'model': (entry / 'model').read_text().strip(),
                   'bytes': int((disk / 'size').read_text()) * 512,
                   'pci': pci.name, 'boot_id': boot}
    else:
        if not state or state['serial'] != args.serial or state['boot_id'] != boot:
            raise RuntimeError('cannot establish device identity for this boot')
        current = state
        pci = Path('/sys/bus/pci/devices') / current['pci']
    group = (pci / 'iommu_group').resolve()
    if not (group / 'devices').is_dir() or len(list((group / 'devices').iterdir())) != 1:
        raise RuntimeError('target must have its own IOMMU group')
    if args.action == 'status':
        print(json.dumps({**current, 'driver': driver(pci), 'iommu_group': group.name}, indent=2))
        return
    if args.action == 'stop':
        vfio = '/dev/vfio/' + group.name
        if driver(pci) == 'vfio-pci' and run('fuser', vfio, check=False).returncode == 0:
            raise RuntimeError('SPDK still owns the VFIO group; stop its process first')
        bind(pci, 'nvme')
        wait_for(lambda: kernel_device(args.serial))
        print('kernel nvme driver restored for ' + args.serial)
        return
    if found:
        unused(disk)
    hugepages = Path('/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages')
    if int(hugepages.read_text()) < 256:
        hugepages.write_text('256')
    if int(hugepages.read_text()) < 256:
        raise RuntimeError('could not allocate 512 MiB of 2-MiB hugepages')
    mount = Path('/dev/hugepages')
    mount.mkdir(exist_ok=True)
    mount_info = run('findmnt', '-n', '-o', 'FSTYPE', '--target', str(mount), check=False)
    if mount_info.stdout.strip() != 'hugetlbfs':
        run('mount', '-t', 'hugetlbfs', '-o', 'pagesize=2M', 'hugetlbfs', str(mount))
    os.chown(mount, 0, account.pw_gid)
    mount.chmod(0o1770)
    # SPDK's PCI uevent listener asks for a 1-MiB SO_RCVBUF (Linux doubles it).
    rmem = Path('/proc/sys/net/core/rmem_max')
    if int(rmem.read_text()) < 2097152:
        rmem.write_text('2097152')
    # Store the verified identity before changing drivers, including the boot ID.
    state_file.write_text(json.dumps(current, indent=2) + '\n')
    try:
        bind(pci, 'vfio-pci')
        vfio = Path('/dev/vfio') / group.name
        wait_for(vfio.exists)
        os.chown(vfio, account.pw_uid, account.pw_gid)
        vfio.chmod(0o600)
        config_dir = Path('/etc/kvfabric')
        config_dir.mkdir(mode=0o755, exist_ok=True)
        config = {'subsystems': [
            {'subsystem': 'iobuf', 'config': [{'method': 'iobuf_set_options', 'params': {
                'small_pool_count': 1024, 'large_pool_count': 64}}]},
            {'subsystem': 'bdev', 'config': [
                {'method': 'bdev_set_options', 'params': {
                    'bdev_io_pool_size': 1024, 'bdev_io_cache_size': 64}},
                {'method': 'bdev_nvme_attach_controller', 'params': {
                    'name': 'Samsung', 'trtype': 'PCIe', 'traddr': pci.name}}]}]}
        (config_dir / 'spdk-samsung.json').write_text(json.dumps(config, indent=2) + '\n')
    except Exception:
        bind(pci, 'nvme')
        raise
    print(json.dumps({**current, 'driver': driver(pci), 'iommu_group': group.name}, indent=2))


if __name__ == '__main__':
    main()
