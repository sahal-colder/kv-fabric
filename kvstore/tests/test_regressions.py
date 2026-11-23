"""Run existing clients and runtime persistence checks in isolated processes."""

import subprocess
import tempfile
from pathlib import Path

from test_sync_e2e import ROOT, Server, eventually, free_port, request


def main():
    sync_port = free_port()
    server = Server(
        free_port(),
        "--persistence",
        "full",
        "--role",
        "master",
        "--replication-port",
        str(sync_port),
    )
    follower = None
    try:
        for name in ("array", "rbtree", "hash", "skiptable", "protocol", "redis"):
            completed = subprocess.run(
                [str(ROOT / f"build/bin/test_{name}"), "127.0.0.1", str(server.port)],
                capture_output=True,
                check=False,
                text=True,
                timeout=20,
                cwd=ROOT,
            )
            assert completed.returncode == 0, completed.stdout + completed.stderr
            print(completed.stdout.strip().splitlines()[-1])
        with tempfile.TemporaryDirectory(prefix="kvstore-full-") as directory:
            path = str(Path(directory) / "snapshot")
            assert request(server.port, "SET", "checkpoint", "original") == b"OK"
            assert request(server.port, "SAVE", path) == b"OK"
            follower = Server(
                free_port(),
                "--role",
                "slave",
                "--master-host",
                "127.0.0.1",
                "--master-port",
                str(sync_port),
            )
            eventually(
                lambda: request(follower.port, "GET", "checkpoint") == b"original"
            )
            assert request(server.port, "SET", "checkpoint", "new") == b"OK"
            eventually(lambda: request(follower.port, "GET", "checkpoint") == b"new")
            assert request(server.port, "LOAD", path) == b"OK"
            assert request(server.port, "GET", "checkpoint") == b"original"
            eventually(
                lambda: request(follower.port, "GET", "checkpoint") == b"original"
            )
        print("runtime full persistence / LOAD resync: PASS")
    finally:
        if follower:
            follower.stop()
        server.stop()
    with tempfile.TemporaryDirectory(prefix="kvstore-aof-") as directory:
        server = Server(
            free_port(),
            "--persistence",
            "aof",
            "--aof-file",
            str(Path(directory) / "events.aof"),
        )
        try:
            assert request(server.port, "SET", "checkpoint", "original") == b"OK"
            assert request(server.port, "SAVE") == b"OK"
            assert request(server.port, "LOAD") == b"OK"
            assert request(server.port, "GET", "checkpoint") == b"original"
            print("runtime AOF persistence: PASS")
        finally:
            server.stop()
    server = Server(free_port(), "--network", "proactor")
    try:
        assert request(server.port, "SET", "proactor", "ok") == b"OK"
        assert request(server.port, "GET", "proactor") == b"ok"
        print("runtime Proactor selection/shutdown: PASS")
    finally:
        server.stop()


if __name__ == "__main__":
    main()
