#!/usr/bin/env python3
"""Byte-verified TCP/strict rsockets pairs through the production Worker CLI.

TRANSFER timing covers directory lookup, connection acquisition, source
snapshot, CRC, receive and destination commit. It is not raw link bandwidth,
process startup latency, LLM TTFT, or a GPUDirect benchmark.
"""
import argparse
from contextlib import ExitStack
import csv
import json
import math
import os
from pathlib import Path
import re
import select
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", type=Path, required=True)
    ap.add_argument("--address", required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--rounds", type=int, default=30)
    ap.add_argument("--devices", nargs="+", choices=("cpu", "cuda"), default=("cpu", "cuda"))
    ap.add_argument("--transports", nargs="+", choices=("tcp", "rdma"), default=("tcp", "rdma"))
    ap.add_argument("--layer-bytes", nargs="+", type=int, default=(65536, 1048576, 4194304))
    a = ap.parse_args()
    a.build = a.build.resolve()
    a.out.mkdir(parents=True, exist_ok=False)
    if a.rounds < 1:
        ap.error("rounds must be positive")
    records = []
    env = dict(os.environ, MINIINFER_WORKER_TIMING="1")

    def start(stack, command, name):
        f = stack.enter_context((a.out / (name + ".stderr")).open("w"))
        child = subprocess.Popen(list(map(str,command)), stdout=subprocess.PIPE, stderr=f, text=True, env=env)
        def stop():
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill(); child.wait()
        stack.callback(stop)
        if not select.select([child.stdout], [], [], 20)[0]:
            raise RuntimeError(name + " startup timeout")
        ready = child.stdout.readline()
        if not ready.startswith("READY "):
            raise RuntimeError(name + " startup failed; inspect stderr")
        return ready.split()[1]

    with (a.out / "raw.csv").open("w") as raw, tempfile.TemporaryDirectory(prefix="kvf-rdma-payload-") as tmp:
        writer = None
        for layer_bytes in a.layer_bytes:
            fixture = Path(tmp) / str(layer_bytes)
            fixture.mkdir()
            subprocess.run([str(a.build / "bin/kvf_bench"), "fixture", str(layer_bytes), str(fixture)], check=True)
            expected = (fixture / "tensors").read_bytes()
            for device in a.devices:
                with ExitStack() as stack:
                    controls = {}
                    for mode in a.transports:
                        tag = f"{device}-{layer_bytes}-{mode}"
                        controls[mode] = start(stack, [a.build / "bin/kvfabricd", "--listen", "127.0.0.1:0"], tag + "-directory")
                        start(stack, [a.build / "bin/miniinfer_worker", "serve", "source-" + mode,
                            controls[mode], a.address + ":0", device, fixture / "namespace",
                            fixture / "tokens", fixture / "tensors", "--kv-transport", mode,
                            "--no-kv-rdma-fallback"], tag + "-source")
                    for sample in range(a.rounds):
                        order = a.transports if sample % 2 == 0 else list(reversed(a.transports))
                        for position, mode in enumerate(order):
                            dest = Path(tmp) / "received"
                            command = [a.build / "bin/miniinfer_worker", "pull", f"sink-{sample}-{mode}",
                                controls[mode], a.address + ":0", device, fixture / "namespace",
                                fixture / "tokens", dest, "--kv-transport", mode, "--no-kv-rdma-fallback"]
                            result = subprocess.run(list(map(str, command)), capture_output=True, text=True, env=env, timeout=30)
                            log = f"{device}-{layer_bytes}-{sample}-{mode}.log"
                            (a.out / log).write_text(result.stdout + result.stderr)
                            active = "TCP" if mode == "tcp" else "RDMA_RSOCKET"
                            assert result.returncode == 0, log
                            assert "active_transport: " + active in result.stderr, log
                            assert dest.read_bytes() == expected, log
                            match = re.search(r"TRANSFER (\d+) (\d+)", result.stderr)
                            assert match and int(match[2]) == len(expected), log
                            us = int(match[1])
                            row = dict(device=device, layer_bytes=layer_bytes, payload_bytes=len(expected),
                                sample=sample, position=position, active_transport=active, transfer_us=us,
                                effective_payload_GiB_s=len(expected) / 2**30 / (us * 1e-6), verified=True, log=log)
                            if writer is None:
                                writer = csv.DictWriter(raw, fieldnames=list(row)); writer.writeheader()
                            writer.writerow(row);raw.flush();records.append(row)
    groups = {}
    for row in records:
        groups.setdefault((row["device"], row["payload_bytes"], row["active_transport"]), []).append(row["transfer_us"])
    summary = []
    for (device, payload, transport), values in groups.items():
        values.sort()
        quant = {name: values[math.ceil(q * len(values))-1] for name,q in (("p50",.5),("p95",.95),("p99",.99))}
        summary.append(dict(device=device, payload_bytes=payload, active_transport=transport, n=len(values),
                            transfer_us=quant, effective_payload_GiB_s_at_p50=payload/2**30/(quant["p50"]*1e-6)))
    (a.out / "summary.json").write_text(json.dumps({"scope": __doc__, "groups": summary}, indent=2))
    print(json.dumps(summary,indent=2))


if __name__ == "__main__":
    main()
