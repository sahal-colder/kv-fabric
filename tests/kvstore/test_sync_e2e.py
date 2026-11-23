"""Self-contained kvstore tests. Does not import or execute MiniInfer."""

import argparse
import contextlib
import os
import resource
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def request(port, *args):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as conn:
        payload = [arg.encode() if isinstance(arg, str) else arg for arg in args]
        conn.sendall(
            b"*%d\r\n" % len(payload)
            + b"".join(b"$%d\r\n" % len(arg) + arg + b"\r\n" for arg in payload)
        )
        with conn.makefile("rb") as stream:
            line = stream.readline()
            if line[:1] == b"$":
                size = int(line[1:-2])
                if size == -1:
                    return None
                value = stream.read(size)
                assert stream.read(2) == b"\r\n"
                return value
            if line[:1] == b":":
                return int(line[1:-2])
            return line[1:-2]


def free_port():
    with socket.socket() as conn:
        conn.bind(("127.0.0.1", 0))
        return conn.getsockname()[1]


def eventually(check, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if check():
                return
        except (OSError, AssertionError):
            pass
        time.sleep(0.04)
    raise AssertionError("condition did not become true")


class Server:
    def __init__(self, port, *args, file_limit=None):
        self.port = port
        self.resources = contextlib.ExitStack()
        self.log = self.resources.enter_context(
            tempfile.TemporaryFile(mode="w+b")  # noqa: SIM115 - owned by resources until stop()
        )
        def limit_file():
            resource.setrlimit(resource.RLIMIT_FSIZE, (file_limit, file_limit))

        self.process = subprocess.Popen(
            [
                str(ROOT / "build/bin/kvstore"),
                str(port),
                "hash",
                "--persistence",
                "none",
                "--sync-reconnect-ms",
                "100",
                "--network",
                os.environ.get("KVS_TEST_NETWORK", "reactor"),
                *args,
            ],
            cwd=ROOT,
            stdout=self.log,
            stderr=self.log,
            preexec_fn=limit_file if file_limit is not None else None,
        )
        try:
            eventually(
                lambda: (
                    self.process.poll() is None and request(port, "PING") == b"PONG"
                ),
                5,
            )
        except (OSError, AssertionError):
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.log.seek(0)
            message = self.log.read().decode(errors="replace")
            self.resources.close()
            raise AssertionError(message) from None

    def stop(self, expected=0):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGCONT)
            self.process.terminate()
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise AssertionError("server did not shut down within its deadline")
        self.log.seek(0)
        message = self.log.read().decode(errors="replace")
        self.resources.close()
        assert self.process.returncode == expected, (self.process.returncode, message)
        assert "ERROR: AddressSanitizer" not in message and "runtime error:" not in message, message


def exercise(dedicated, bpf_map=None):
    master_port, slave_port, sync_port = free_port(), free_port(), free_port()
    extra = ["--ebpf-enabled", "yes", "--ebpf-map", bpf_map] if bpf_map else []
    master_args = ["--role", "master", *extra]
    if dedicated:
        master_args += [
            "--replication-port",
            str(sync_port),
            "--sync-queue-bytes",
            "4096",
        ]
    leader = Server(master_port, *master_args)
    follower = None
    try:
        # The initial snapshot exceeds the old 1 KiB network limit.
        for i in range(120):
            assert request(master_port, "SET", f"before:{i}", "value" * 30) == b"OK"
        follower = Server(
            slave_port,
            "--role",
            "slave",
            "--master-host",
            "127.0.0.1",
            "--master-port",
            str(sync_port if dedicated else master_port),
            *extra,
        )
        eventually(lambda: request(slave_port, "GET", "before:119") == b"value" * 30)
        assert request(slave_port, "SET", "forbidden", "x").startswith(b"READONLY")
        # Legacy engine writes and Redis writes retain their separate indexes.
        for prefix in (b"", b"R", b"H", b"S"):
            with socket.create_connection(
                ("127.0.0.1", master_port), timeout=3
            ) as conn:
                conn.sendall(prefix + b"SET legacy value\r\n")
                assert conn.recv(128) == b"OK\r\n"

            def found(prefix=prefix):
                with socket.create_connection(
                    ("127.0.0.1", slave_port), timeout=3
                ) as conn:
                    conn.sendall(prefix + b"GET legacy\r\n")
                    return conn.recv(128) == b"value\r\n"

            eventually(found)
        for i in range(100):
            assert request(master_port, "SET", "after", str(i)) == b"OK"
        eventually(lambda: request(slave_port, "GET", "after") == b"99")
        assert request(master_port, "DEL", "before:0") == 1
        eventually(lambda: request(slave_port, "GET", "before:0") is None)
        eventually(lambda: b"connected_slaves:1" in request(master_port, "INFO"))
        if dedicated:
            # A paused replica must not block writes or grow the delta queue forever.
            follower.process.send_signal(signal.SIGSTOP)
            try:
                with (
                    socket.create_connection(
                        ("127.0.0.1", master_port), timeout=3
                    ) as conn,
                    conn.makefile("rb") as stream,
                ):
                    for i in range(15000):
                        value = f"{i:05d}".encode() + b"x" * 500
                        wire = (
                            b"*3\r\n$3\r\nSET\r\n$4\r\nslow\r\n$505\r\n"
                            + value
                            + b"\r\n"
                        )
                        conn.sendall(wire)
                        assert stream.read(5) == b"+OK\r\n"
                eventually(lambda: b"connected_slaves:0" in request(master_port, "INFO"))
            finally:
                follower.process.send_signal(signal.SIGCONT)
            eventually(
                lambda: request(slave_port, "GET", "slow") == b"14999" + b"x" * 500
            )
        # A restart forces a new snapshot and removes data absent from the new master.
        leader.stop()
        eventually(lambda: b"master_link_status:down" in request(slave_port, "INFO"))
        time.sleep(0.7)  # More than the old three-attempt reconnect limit.
        leader = Server(master_port, *master_args)
        assert request(master_port, "SET", "new-epoch", "new") == b"OK"
        eventually(lambda: request(slave_port, "GET", "new-epoch") == b"new")
        assert request(slave_port, "GET", "after") is None
        assert request(master_port, "FLUSHDB") == b"OK"
        eventually(lambda: request(slave_port, "DBSIZE") == 0)
        # Heartbeats keep an otherwise idle connection alive.
        time.sleep(2.2)
        assert b"master_link_status:up" in request(slave_port, "INFO")
        print(f"sync e2e: PASS (dedicated={dedicated}, ebpf={bool(bpf_map)})")
    finally:
        if follower:
            follower.stop()
        leader.stop()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--ebpf",
        action="store_true",
        help="requires root; mounts an isolated temporary bpffs",
    )
    args = parser.parse_args()
    if not args.ebpf:
        exercise(False)
        exercise(True)
        return
    if os.geteuid() != 0:
        parser.error("--ebpf requires root")
    with tempfile.TemporaryDirectory(prefix="kvstore-bpffs-") as directory:
        subprocess.run(["mount", "-t", "bpf", "bpf", directory], check=True)
        try:
            exercise(True, str(Path(directory) / "sync"))
        finally:
            with contextlib.suppress(FileNotFoundError):
                (Path(directory) / "sync").unlink()
            subprocess.run(["umount", directory], check=True)


if __name__ == "__main__":
    main()
