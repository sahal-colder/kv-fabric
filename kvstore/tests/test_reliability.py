"""Socket, disk fault, and restart tests against the real server."""

import concurrent.futures
import contextlib
import os
import signal
import socket
import tempfile
from pathlib import Path

from test_sync_e2e import Server, eventually, free_port, request


def resp(*args):
    parts = [arg if isinstance(arg, bytes) else arg.encode() for arg in args]
    return b"*%d\r\n" % len(parts) + b"".join(
        b"$%d\r\n" % len(part) + part + b"\r\n" for part in parts
    )


def connect(port):
    return socket.create_connection(("127.0.0.1", port), timeout=5)


def expect_no_reply(conn):
    conn.settimeout(0.10)
    try:
        conn.recv(1)
    except TimeoutError:
        return
    finally:
        conn.settimeout(5)
    raise AssertionError("incomplete request produced a response or closed the connection")


def exercise_network(backend):
    server = Server(free_port(), "--network", backend)
    port = server.port
    stop_status = -signal.SIGTERM if backend == "ntyco" else 0
    with contextlib.ExitStack() as clients:
        try:
            with connect(port) as conn, conn.makefile("rb") as stream:
                conn.sendall(b"SET split val")
                expect_no_reply(conn)
                conn.sendall(b"ue\r\nGET split\r\n")
                assert stream.readline() == b"OK\r\n"
                assert stream.readline() == b"value\r\n"

                next_command = resp("SET", "fragment", "works")
                conn.sendall(resp("PING") + next_command[:9])
                assert stream.readline() == b"+PONG\r\n"
                conn.sendall(next_command[9:])
                assert stream.readline() == b"+OK\r\n"

                conn.sendall(resp("SET", "bad\x00key", "value") + resp("GET", "bad"))
                assert stream.readline().startswith(b"-ERR")
                assert stream.readline() == b"$-1\r\n"
                conn.sendall(resp("ECHO", b"a\0b"))
                assert stream.read(9) == b"$3\r\na\0b\r\n"

                conn.sendall(resp("PING") * 2048)
                assert stream.read(7 * 2048) == b"+PONG\r\n" * 2048
                conn.sendall(resp("GET", "fragment"))
                conn.shutdown(socket.SHUT_WR)
                assert stream.read() == b"$5\r\nworks\r\n"

            with connect(port) as conn:
                conn.sendall(b"x" * (64 * 1024))
                assert not conn.recv(1)

            # More parked clients than the submission ring has SQ slots.
            for _ in range(1200 if backend != "ntyco" else 100):
                clients.enter_context(connect(port))

            def worker(worker_id):
                key = f"worker:{worker_id}"
                with connect(port) as conn, conn.makefile("rb") as stream:
                    for i in range(64):
                        value = str(i)
                        conn.sendall(resp("SET", key, value) + resp("GET", key) + resp("INCR", "total"))
                        assert stream.readline() == b"+OK\r\n"
                        assert stream.readline() == b"$%d\r\n" % len(value)
                        assert stream.readline().strip() == value.encode()
                        assert int(stream.readline()[1:]) > 0

            with concurrent.futures.ThreadPoolExecutor(max_workers=32) as pool:
                list(pool.map(worker, range(32)))
            assert request(port, "GET", "total") == b"2048"

            assert request(port, "SET", "large", b"x" * 900) == b"OK"
            slow = clients.enter_context(socket.socket())
            slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            slow.settimeout(5)
            slow.connect(("127.0.0.1", port))
            slow.sendall(resp("GET", "large") * 10000)
            assert request(port, "PING") == b"PONG"
        finally:
            # Keep the parked clients and the stalled receiver open through shutdown.
            server.stop(expected=stop_status)
    print(f"network framing, pipeline, concurrency, slow reader and shutdown: PASS ({backend})")


def exercise_aof(directory):
    path = directory / "events.aof"
    config = ("--persistence", "aof", "--aof-file", str(path))
    server = Server(free_port(), *config)
    try:
        assert request(server.port, "SET", "key", "old") == b"OK"
        assert request(server.port, "SAVE") == b"OK"
        server.process.kill()
        server.process.wait()
    finally:
        server.stop(expected=-signal.SIGKILL)
    saved = path.read_bytes()
    path.write_bytes(saved + resp("SET", "incomplete", "ignored")[:-3])
    server = Server(free_port(), *config)
    try:
        assert request(server.port, "GET", "key") == b"old"
        assert request(server.port, "GET", "incomplete") is None
        assert path.read_bytes() == saved
        assert request(server.port, "SET", "live-only", "keep") == b"OK"
        with path.open("r+b") as stream:
            stream.truncate(len(saved))
        for corrupt in (resp("MOD", "absent", "v"), resp("SET", b"k\0y", "v"), b"*x"):
            with path.open("ab") as stream:
                stream.write(corrupt)
            assert request(server.port, "LOAD") == b"ERR load failed"
            assert request(server.port, "GET", "key") == b"old"
            assert request(server.port, "GET", "live-only") == b"keep"
            assert path.read_bytes() == saved + corrupt
            with path.open("r+b") as stream:
                stream.truncate(len(saved))
    finally:
        server.stop()

    server = Server(free_port(), *config, file_limit=len(saved) + 12)
    try:
        with connect(server.port) as conn:
            conn.sendall(resp("SET", "key", "new"))
            assert conn.recv(32) == b""
        assert server.process.wait(timeout=5) == 1
    finally:
        server.stop(expected=1)
    assert path.read_bytes() == saved
    server = Server(free_port(), *config)
    try:
        assert request(server.port, "GET", "key") == b"old"
    finally:
        server.stop()
    print("AOF crash restart, incomplete tail, bad replay and short write: PASS")


def exercise_snapshot(directory):
    path = directory / "snapshot"
    server = Server(free_port(), "--persistence", "full")
    try:
        assert request(server.port, "SET", "key", "before") == b"OK"
        assert request(server.port, "SAVE", str(path)) == b"OK"
    finally:
        server.stop()
    saved = path.read_bytes()
    assert b"before" in saved
    server = Server(free_port(), "--persistence", "full", file_limit=len(saved) + 8)
    try:
        assert request(server.port, "SET", "key", b"x" * 500) == b"OK"
        assert request(server.port, "SAVE", str(path)) == b"ERR save failed"
        assert path.read_bytes() == saved
        assert not list(directory.glob(".kvs-*"))
        bad = directory / "bad.snapshot"
        for content in (saved[:-1], saved.replace(b"before", b"damage")):
            bad.write_bytes(content)
            assert request(server.port, "LOAD", str(bad)) == b"ERR load failed"
            assert request(server.port, "GET", "key") == b"x" * 500
        assert request(server.port, "LOAD", str(path)) == b"OK"
        assert request(server.port, "GET", "key") == b"before"
    finally:
        server.stop()
    print("snapshot short write, atomic replacement, truncation and CRC: PASS")


def exercise_replica_aof(directory):
    primary_port, sync_port = free_port(), free_port()
    primary_config = ("--role", "master", "--replication-port", str(sync_port))
    follower_config = (
        "--role", "slave", "--master-host", "127.0.0.1", "--master-port", str(sync_port),
        "--persistence", "aof", "--aof-file", str(directory / "replica.aof"),
    )
    primary = follower = None
    try:
        primary = Server(primary_port, *primary_config)
        assert request(primary_port, "SET", "stale", "remove-me") == b"OK"
        follower = Server(free_port(), *follower_config)
        eventually(lambda: request(follower.port, "GET", "stale") == b"remove-me")
        primary.stop()
        primary = None
        eventually(lambda: b"master_link_status:down" in request(follower.port, "INFO"))

        primary = Server(primary_port, *primary_config)
        assert request(primary_port, "SET", "current", "new") == b"OK"
        eventually(lambda: request(follower.port, "GET", "current") == b"new")
        assert request(follower.port, "GET", "stale") is None
        assert request(primary_port, "SET", "delta", "after") == b"OK"
        eventually(lambda: request(follower.port, "GET", "delta") == b"after")
        assert request(follower.port, "SAVE") == b"OK"
        primary.stop()
        primary = None
        follower.stop()
        follower = Server(free_port(), *follower_config)
        assert request(follower.port, "GET", "stale") is None
        assert request(follower.port, "GET", "current") == b"new"
        assert request(follower.port, "GET", "delta") == b"after"
        follower.stop()
        follower = None

        # Reject a full sync whose checkpoint exceeds the file write limit.
        log = directory / "replica.aof"
        saved = log.read_bytes()
        primary = Server(primary_port, *primary_config)
        assert request(primary_port, "SET", "larger", "x" * 500) == b"OK"
        follower = Server(free_port(), *follower_config, file_limit=len(saved) + 8)
        def failed_checkpoint():
            return (b"replica AOF replacement" in os.pread(follower.log.fileno(), 16384, 0) and
                    b"master_link_status:down" in request(follower.port, "INFO"))
        eventually(failed_checkpoint)
        assert request(follower.port, "GET", "current") == b"new"
        assert request(follower.port, "GET", "larger") is None
        assert log.read_bytes() == saved
    finally:
        if follower:
            follower.stop()
        if primary:
            primary.stop()
    print("replica full resync and offline AOF restart: PASS")


if __name__ == "__main__":
    exercise_network(os.environ.get("KVS_TEST_NETWORK", "reactor"))
    if os.environ.get("KVS_TEST_NETWORK") != "ntyco":
        with tempfile.TemporaryDirectory(prefix="kvstore-reliability-") as directory:
            exercise_aof(Path(directory))
            exercise_snapshot(Path(directory))
            exercise_replica_aof(Path(directory))
