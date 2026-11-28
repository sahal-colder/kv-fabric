"""Real Legacy server security, large values, and authenticated replication."""
import os
from pathlib import Path
import socket
import subprocess

from test_sync_e2e import Server, eventually, free_port, request, ROOT, TEST_BUILD, TEST_SECRET
import hashlib
import hmac


def closed_without_data(conn):
    try:
        assert conn.recv(1) == b""
    except ConnectionResetError:
        pass  # TCP may reset when closing a rejected handshake with unread bytes.


def test_remote_files_disabled(tmp_path):
    server=Server(free_port(),"--persistence","full")
    try:
        sentinel=tmp_path/"sentinel";sentinel.write_bytes(b"unchanged")
        link=tmp_path/"symlink";link.symlink_to(sentinel)
        for path in [sentinel,link,tmp_path/"../escape",Path("/etc/passwd")]:
            for command in ["SAVE","LOAD"]:
                assert request(server.port,command,str(path)).startswith(b"NOPERM")
                with socket.create_connection(("127.0.0.1",server.port),timeout=3) as conn:
                    conn.sendall(f"{command} {path}\r\n".encode())
                    assert b"disabled" in conn.recv(1024)
        assert sentinel.read_bytes()==b"unchanged" and not (tmp_path.parent/"escape").exists()
        assert request(server.port,"SAVE").startswith(b"NOPERM")
    finally:server.stop()


def test_large_values_and_multi_key():
    server=Server(free_port())
    try:
        value=b"x"*32768
        assert request(server.port,"SET","large-audit",value)==b"OK"
        assert request(server.port,"GET","large-audit")==value
        assert request(server.port,"MSET","aa","one","bb","two","aa","last")==b"OK"
        assert request(server.port,"GET","aa")==b"last"
        assert request(server.port,"MSETNX","aa","bad","cc","bad")==0
        assert request(server.port,"GET","aa")==b"last" and request(server.port,"GET","cc") is None
        assert request(server.port,"DEL","aa","aa","bb")==2
        assert request(server.port,"UNLINK","large-audit","absent")==1
    finally:server.stop()


def test_client_regressions():
    server=Server(free_port())
    try:
        for name in ["array","rbtree","hash","skiptable","protocol","redis"]:
            p=subprocess.run([str(TEST_BUILD/f"bin/test_{name}"),"127.0.0.1",str(server.port)],cwd=ROOT,
                             stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=30)
            assert p.returncode==0,p.stdout
    finally:server.stop()


def test_authentication_before_snapshot_and_slots():
    port=free_port();leader=Server(free_port(),"--role","master","--replication-port",str(port))
    follower=None
    try:
        assert request(leader.port,"SET","secret-data","protected")==b"OK"
        for _ in range(70):
            with socket.create_connection(("127.0.0.1",port),timeout=3) as conn:
                conn.sendall(b"KVS_SYNC_HELLO_V1\r\n")
                closed_without_data(conn)  # no V1 ACK, snapshot, or peer allocation
        assert b"connected_slaves:0" in request(leader.port,"INFO")
        # Valid V2 framing with an incorrect proof receives no snapshot.
        with socket.create_connection(("127.0.0.1",port),timeout=3) as conn:
            conn.sendall(b"KVS_SYNC_AUTH_V2\r\n");challenge=conn.recv(32);assert len(challenge)==32
            conn.sendall(b"a"*64);closed_without_data(conn)
        assert b"connected_slaves:0" in request(leader.port,"INFO")
        # A captured valid proof is not valid under a new server nonce.
        def receive_exact(conn, size):
            data=b""
            while len(data)<size:
                part=conn.recv(size-len(data));assert part;data+=part
            return data
        with socket.create_connection(("127.0.0.1",port),timeout=3) as conn:
            conn.sendall(b"KVS_SYNC_AUTH_V2\r\n")
            nonce=receive_exact(conn,32);client_nonce=os.urandom(32)
            captured=client_nonce+hmac.digest(TEST_SECRET.encode(),b"C"+nonce+client_nonce,"sha256")
            conn.sendall(captured)
            proof=receive_exact(conn,32)
            assert hmac.compare_digest(proof,hmac.digest(TEST_SECRET.encode(),b"S"+nonce+client_nonce,"sha256"))
        with socket.create_connection(("127.0.0.1",port),timeout=3) as conn:
            conn.sendall(b"KVS_SYNC_AUTH_V2\r\n")
            new_nonce=receive_exact(conn,32);assert new_nonce!=nonce
            conn.sendall(captured);closed_without_data(conn)
        eventually(lambda:b"connected_slaves:0" in request(leader.port,"INFO"))
        follower=Server(free_port(),"--role","slave","--master-host","127.0.0.1","--master-port",str(port))
        eventually(lambda:request(follower.port,"GET","secret-data")==b"protected")
        assert b"connected_slaves:1" in request(leader.port,"INFO")
        assert request(leader.port,"MSET","a","v1","b","v2")==b"OK"
        eventually(lambda:request(follower.port,"GET","b")==b"v2")
        assert request(follower.port,"SET","forbidden","v").startswith(b"READONLY")
    finally:
        if follower:follower.stop()
        leader.stop()


def test_missing_credentials_rejected():
    p=subprocess.run([str(TEST_BUILD/"bin/kvstore"),str(free_port()),"hash","--role","master",
                      "--replication-port",str(free_port())],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                     text=True,timeout=5)
    assert p.returncode!=0 and "replication-secret" in p.stdout
