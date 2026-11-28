#!/usr/bin/env python3
"""Real fixed-buffer parallel owned reads: full batch, failed suffix, CRC and timeout."""
import argparse
import ctypes
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import random
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "scripts"), str(ROOT / "integrations/vllm")]
from bench_vllm_kv import directory
from kvfabric_vllm.bridge import Bridge, Spec


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("build", type=Path)
    p.add_argument("out", type=Path)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    rng = random.Random(613)
    with directory(a.build / "bin/kvfabricd", a.out / "directory.log") as control:
        spec = Spec(control.encode(), b"owned-disk-parallel-test", b"test", 16, 36, 16384)
        b = Bridge(str(a.build / "src/integration/libkvf_vllm.so"), spec)
        path = a.out / "owned.bin"
        try:
            assert b.open("owned-disk-test", os.fsencode(path), 32, 4) == "direct-uring-fixed"
            ids, hashes = b.chain([rng.randrange(20000) for _ in range(8 * 16)])
            size = 8 * spec.layers * spec.layer_bytes
            data = rng.randbytes(size)
            source = ctypes.create_string_buffer(data, size)
            dest = ctypes.create_string_buffer(size)
            assert b.transfer(ids, hashes, 0, 8, ctypes.addressof(source)) == 0
            deadline = time.monotonic() + 5
            while b.lookup(hashes, 0, 8, 1000) != 8:
                assert time.monotonic() < deadline
                time.sleep(.01)
            for _ in range(8):
                assert b.load_owned(hashes, 0, 8, 1000, ctypes.addressof(dest)) == 8
                assert dest.raw == data
            old = bytes(hashes[5])
            hashes[5][:] = bytes([173]) * 32
            assert b.load_owned(hashes, 0, 8, 1000, ctypes.addressof(dest)) == 4
            assert dest.raw[:4 * size // 8] == data[:4 * size // 8]
            hashes[5][:] = old
            assert b.load_owned(hashes, 0, 8, 0, ctypes.addressof(dest)) == 0
            # Foreground reads must leave capacity for the one background publisher.
            def reads():
                for _ in range(32):
                    assert b.load_owned(hashes, 0, 8, 1000, ctypes.addressof(dest)) == 8
                    assert dest.raw == data
            def writes():
                for _ in range(3):
                    other_ids, other_hashes = b.chain([rng.randrange(20000) for _ in range(8 * 16)])
                    assert b.transfer(other_ids, other_hashes, 0, 8, ctypes.addressof(source)) == 0
            with ThreadPoolExecutor(max_workers=2) as pool:
                pending = [pool.submit(reads), pool.submit(writes)]
                for future in pending:
                    future.result()
            # Every persisted record has a bad payload, with its header intact.
            fd = os.open(path, os.O_RDWR)
            try:
                for offset in range(4096, os.fstat(fd).st_size, 20480):
                    byte = os.pread(fd, 1, offset)
                    if byte:
                        os.pwrite(fd, bytes([byte[0] ^ 1]), offset)
                os.fsync(fd)
            finally:
                os.close(fd)
            assert b.load_owned(hashes, 0, 8, 1000, ctypes.addressof(dest)) == 0
            print(json.dumps({"passed": True, "batch_pages": 8, "read_lanes": 4,
                              "verified_full_batch_bytes": size * 40,
                              "cases": ["full batch", "first failed page returns prefix only",
                                        "zero timeout", "concurrent background publication",
                                        "persisted CRC corruption"]}))
        finally:
            b.close()
            path.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
