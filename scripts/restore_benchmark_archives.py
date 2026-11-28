#!/usr/bin/env python3
"""Restore losslessly archived benchmark files and verify original SHA-256.

No network, model download, device access or hardware writes are performed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile
import zlib


def checked_path(root, value):
    target = (root/value).resolve()
    if not target.is_relative_to(root):
        raise ValueError('archive path escapes repository: '+value)
    return target


def restore(root, entry):
    target = checked_path(root, entry['original_path'])
    if target.is_file():
        if target.stat().st_size == entry['original_bytes'] and hashlib.sha256(target.read_bytes()).hexdigest() == entry['original_sha256']:
            return False
        raise RuntimeError('existing file differs; refusing overwrite: '+str(target))
    decoder = zlib.decompressobj(16+zlib.MAX_WBITS)
    digest = hashlib.sha256()
    total = 0
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.restore-', dir=target.parent)
    try:
        with os.fdopen(fd, 'wb') as output:
            for part in entry['parts']:
                source = checked_path(root, part['path'])
                if not source.is_file():
                    raise RuntimeError('missing release part '+part['path']+'; download and extract the assets listed in docs/benchmarks/release-assets.json first')
                part_hash = hashlib.sha256()
                with source.open('rb') as compressed:
                    while chunk := compressed.read(1024*1024):
                        part_hash.update(chunk)
                        while chunk:
                            data = decoder.decompress(chunk, 1024*1024)
                            chunk = decoder.unconsumed_tail
                            total += len(data)
                            if total > entry['original_bytes']:
                                raise RuntimeError('archive expands beyond recorded size')
                            digest.update(data)
                            output.write(data)
                if part_hash.hexdigest() != part['sha256']:
                    raise RuntimeError('compressed part SHA mismatch: '+str(source))
            tail = decoder.flush()
            total += len(tail)
            output.write(tail)
            digest.update(tail)
        if not decoder.eof or total != entry['original_bytes'] or digest.hexdigest() != entry['original_sha256']:
            raise RuntimeError('original size/SHA mismatch: '+str(target))
        os.replace(temporary, target)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--group', help='Only restore paths containing this string, e.g. spdk-async-2026-11-28')
    parser.add_argument('--list', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    index = json.loads((root/'docs/benchmarks/archive-index.json').read_text())
    selected = [item for item in index['files'] if not args.group or args.group in item['original_path']]
    if args.list:
        for item in selected:
            print(item['original_path'])
        return
    restored = 0
    for item in selected:
        restored += restore(root, item)
    print(f'{len(selected)} originals verified; {restored} files restored')


if __name__ == '__main__':
    main()
