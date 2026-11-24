"""Real Nano-VLLM inference over independent directory, publisher and consumer processes."""

import argparse
import contextlib
import csv
import json
import math
import os
from pathlib import Path
import queue
import random
import re
import shutil
import socket
import socketserver
import struct
import subprocess
import tempfile
import threading
import time

SOURCE = "The quick brown fox"
TARGET = "The quick brown fox jumps high"
OTHER = "A different request runs fast"
PAGE = 4
VOCAB = 260
LAYERS = 2
FETCH_BATCH = 35
PUBLISH = 5


class NoCuda(Exception):
    pass


def fixtures(root, seed=7):
    model = root / f"model-{seed}.bin"
    tokenizer = root / f"tokenizer-{seed}.bin"
    dim, hidden, heads, kv_heads, context = 32, 64, 4, 2, 192
    kv = dim // heads * kv_heads
    randomizer = random.Random(seed)

    def weights(file, n):
        file.write(struct.pack(f"<{n}f", *(randomizer.uniform(-0.24, 0.24) for _ in range(n))))

    with model.open("wb") as file:
        file.write(struct.pack("<7i", dim, hidden, LAYERS, heads, kv_heads, VOCAB, context))
        weights(file, VOCAB * dim)
        file.write(struct.pack(f"<{LAYERS * dim}f", *([1.0] * (LAYERS * dim))))
        for elements in (dim * dim, kv * dim, kv * dim, dim * dim):
            weights(file, LAYERS * elements)
        file.write(struct.pack(f"<{LAYERS * dim}f", *([1.0] * (LAYERS * dim))))
        for _ in range(3):
            weights(file, LAYERS * hidden * dim)
        file.write(struct.pack(f"<{dim}f", *([1.0] * dim)))
        file.write(bytes(context * (dim // heads) * 4))

    vocab = [b"<unk>", b"<s>", b"</s>"] + [f"<0x{i:02X}>".encode() for i in range(256)] + [b" "]
    with tokenizer.open("wb") as file:
        file.write(struct.pack("<i", max(map(len, vocab))))
        for word in vocab:
            file.write(struct.pack("<fi", float(seed), len(word)))
            file.write(word)
    return model, tokenizer


class Child:
    def __init__(self, command, directory=False, env=None, wait_ready=True):
        self.directory = directory
        self.wait_ready = wait_ready
        self.errors = tempfile.TemporaryFile(mode="w+t", encoding="utf-8")
        self.process = subprocess.Popen(
            list(map(str, command)), stdin=subprocess.DEVNULL if directory else subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=self.errors, text=True, bufsize=1, env=env,
        )
        self.output = queue.Queue()
        self.reader = threading.Thread(target=self._read_output, daemon=True)
        self.reader.start()

    def _read_output(self):
        for line in self.process.stdout:
            self.output.put(line.rstrip("\n"))
        self.output.put(None)

    def receive(self):
        try:
            line = self.output.get(timeout=25)
        except queue.Empty as exc:
            raise RuntimeError(f"Timed out waiting for {self.process.args[0]}") from exc
        if line is None:
            code = self.process.wait(timeout=5)
            if code == 77:
                raise NoCuda()
            self.errors.seek(0)
            raise RuntimeError(f"{self.process.args[0]} exited {code}\n{self.errors.read()}")
        return line

    def __enter__(self):
        if not self.wait_ready:
            return self
        try:
            ready = self.receive()
            assert ready == "READY" or (self.directory and ready.startswith("READY ")), ready
            self.endpoint = ready[6:] if self.directory else None
            return self
        except BaseException:
            self.close(check=False)
            raise

    def close(self, check=True, kill=False):
        if self.errors.closed:
            return
        process = self.process
        try:
            if process.poll() is None:
                if kill:
                    process.kill()
                elif self.directory:
                    process.terminate()
                else:
                    process.stdin.close()
            try:
                code = process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise
            if check and code != 0:
                self.errors.seek(0)
                raise RuntimeError(f"{process.args[0]} exited {code}\n{self.errors.read()}")
        finally:
            self.reader.join(timeout=2)
            process.stdout.close()
            if process.stdin is not None:
                process.stdin.close()
            self.errors.close()

    def __exit__(self, exc_type, *_):
        self.close(check=exc_type is None)

    def send(self, line):
        self.process.stdin.write(line + "\n")
        self.process.stdin.flush()

    def infer(self, *prompts, command="infer"):
        self.send("\t".join((command, *prompts)))
        return json.loads(self.receive())


def unused_port():
    with socket.socket() as bound:
        bound.bind(("127.0.0.1", 0))
        return bound.getsockname()[1]


def exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError()
        data.extend(chunk)
    return data


def frame(sock):
    header = exact(sock, 36)
    size, = struct.unpack_from(">I", header, 24)
    if size > 8 * 1024 * 1024:
        raise ValueError("oversized test frame")
    return header, exact(sock, size)


def crc32c(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


class Proxy(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, target):
        self.target = ("127.0.0.1", target)
        self.lock = threading.Lock()
        self.arm("pass")
        super().__init__(("127.0.0.1", 0), Relay)

    def arm(self, mode, after=0):
        with self.lock:
            self.mode, self.after, self.seen, self.injected = mode, after, 0, 0

    def fault(self, opcode):
        with self.lock:
            if self.mode == "publish-drop" and opcode != PUBLISH:
                return "pass"
            count = self.seen
            self.seen += 1
            if self.mode != "pass" and (count == self.after or self.mode == "pace"):
                self.injected += 1
                return self.mode
            return "pass"

    def __enter__(self):
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)
        self.thread.start()
        self.endpoint = f"127.0.0.1:{self.server_address[1]}"
        return self

    def __exit__(self, *_):
        self.shutdown()
        self.thread.join()
        self.server_close()


class Relay(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(3)
        try:
            with socket.create_connection(self.server.target, timeout=3) as peer:
                while True:
                    header, data = frame(self.request)
                    opcode, = struct.unpack_from(">H", header, 8)
                    fault = self.server.fault(opcode)
                    if fault in ("drop", "publish-drop"):
                        return
                    peer.sendall(header + data)
                    header, data = frame(peer)
                    if fault == "crc" and data:
                        assert opcode == FETCH_BATCH
                        # Keep the wire CRC valid so the manifest detects corruption
                        # in page two, after page one has been staged successfully.
                        data[LAYERS * PAGE * 16 * 2 * 4] ^= 1
                        struct.pack_into(">I", header, 28, crc32c(data))
                        struct.pack_into(">I", header, 32, crc32c(header[:32]))
                    if fault == "wire-crc" and data:
                        data[0] ^= 1
                    if fault in ("expired", "slow"):
                        time.sleep(0.60)
                    if fault == "learn":
                        time.sleep(0.08)
                    if fault == "pace":
                        time.sleep(0.18)
                        self.request.sendall(header + data[:len(data) // 2])
                        time.sleep(0.18)
                        self.request.sendall(data[len(data) // 2:])
                        continue
                    self.request.sendall(header + data)
        except (EOFError, OSError):
            return


class Runs:
    def __init__(self, args, root):
        self.args = args
        self.root = root
        self.files = (args.model, args.tokenizer) if args.model else fixtures(root)
        self.env = dict(os.environ, OPENBLAS_NUM_THREADS="1", OMP_NUM_THREADS="1")
        self.name = 0

    def directory(self, ttl=5000):
        conf = self.root / f"directory-{ttl}.conf"
        conf.write_text(f"max_pages=1024\nlease_ttl_ms={ttl}\n", encoding="ascii")
        return Child([self.args.directory, "--config", conf, "--listen", "127.0.0.1:0"], directory=True)

    def worker(self, mode, directory=None, *, tenant="integration", files=None, page=PAGE,
               blocks=48, capacity=48, timeout=2000, new=4, name=None, listen=0, advertise="-", device=None,
               publish_generated=False):
        self.name += 1
        model, tokenizer = files or self.files
        return Child([
            self.args.worker, model, tokenizer, device or self.args.device, mode,
            directory.endpoint if directory else "-", f"127.0.0.1:{listen}", advertise,
            name or f"nano-test-{self.name}", tenant, page, blocks, capacity, timeout, new,
        ], env=dict(self.env, NV_TEST_PUBLISH_GENERATED=str(int(publish_generated))))


def match(output, expected, *, skip=None):
    assert output["tokens"] == expected["tokens"], (output, expected)
    if skip is not None:
        assert output["remote_prefill_skipped_tokens"] == skip, output
        assert output["prefilled"] + output["local"] + skip == expected["prefilled"], output


def positive(runs, directory, cold):
    with runs.worker("remote", directory) as producer:
        published = producer.infer(SOURCE, command="inspect")
        assert published["remote_published_pages"] == 5, published
        with runs.worker("remote", directory) as consumer:
            result = consumer.infer(TARGET, command="inspect")
            match(result, cold[TARGET], skip=20)
            assert result["remote_hit_pages"] == 5
            assert result["remote_lookup_hits"] == result["remote_transfer_samples"] == 5
            assert result["remote_import_bytes"] == 5 * LAYERS * PAGE * 16 * 2 * 4
            assert result["remote_lookup_requests"] == result["data_requests"] == 1, result
            assert result["received_bytes"] == result["remote_import_bytes"], result
            assert result["kv_page_hashes"] == cold[TARGET]["kv_page_hashes"], result
            assert result["kv_page_hashes"][:5] == published["kv_page_hashes"], published
        with runs.worker("remote", directory) as consumer:
            other = consumer.infer(OTHER)
            match(other, cold[OTHER], skip=0)
            assert other["remote_lookup_requests"] and not other["remote_lookup_hits"], other

        alternate_model, alternate_tokenizer = fixtures(runs.root, 13)
        with runs.worker("cold", files=(alternate_model, runs.files[1])) as reference:
            other_model = reference.infer(TARGET)
        for options, expected in (
            ({"tenant": "different"}, cold[TARGET]),
            ({"files": (alternate_model, runs.files[1])}, other_model),
            ({"files": (runs.files[0], alternate_tokenizer)}, cold[TARGET]),
            ({"page": 8}, cold[TARGET]),
        ):
            with runs.worker("remote", directory, **options) as consumer:
                miss = consumer.infer(TARGET)
                match(miss, expected, skip=0)
                assert miss["remote_lookup_requests"] and not miss["remote_lookup_hits"], miss

        if runs.args.device == "cuda":
            with runs.worker("cold", device="cpu") as reference, runs.worker("remote", directory, device="cpu") as consumer:
                miss = consumer.infer(TARGET)
                match(miss, reference.infer(TARGET), skip=0)
                assert miss["remote_lookup_requests"] and not miss["remote_lookup_hits"], miss

        for binary in runs.args.cli:
            base = [str(binary), str(runs.files[0]), "-z", str(runs.files[1]),
                   "-n", "4", "-t", "0.7", "-p", "0.95", "-s", "1234", "--ignore-eos",
                   "--device", runs.args.device]
            prompt = ["-i", TARGET]
            local = subprocess.run(base + prompt + ["--no-cache"], check=True, capture_output=True,
                                   timeout=25, env=runs.env)
            assert local.stdout.startswith(TARGET.encode()) and b"remote_lookup" not in local.stderr, local
            cmd = base + ["--remote-kv", "--kv-page-size", str(PAGE), "--kv-control-endpoint", directory.endpoint]
            for flag in ("--kv-tp-size", "--kv-pp-size"):
                invalid = subprocess.run(cmd + prompt + [flag, "2"], text=True, capture_output=True,
                                         timeout=10, env=runs.env)
                assert invalid.returncode and "TP=1 and PP=1" in invalid.stderr, invalid

            # Independent production CLI workers stay alive after a response.
            cmd += ["--kv-tenant", f"cli-{binary.name}"]
            persistent = cmd + ["--stdin", "--ids"]
            with Child(persistent, env=runs.env, wait_ready=False) as publisher:
                publisher.send(SOURCE)
                assert publisher.receive().startswith("[0] ")
                cli = subprocess.run(cmd + prompt, check=True, capture_output=True, timeout=25, env=runs.env)
                assert cli.stdout == local.stdout, (cli.stdout, local.stdout)
                assert re.search(rb"\bremote_prefill_skipped_tokens=20\b", cli.stderr), cli.stderr
                expected = "[0] " + " ".join(map(str, cold[TARGET]["tokens"][0]))
                with Child(persistent, env=runs.env, wait_ready=False) as consumer:
                    for _ in range(2):
                        consumer.send(TARGET)
                        assert consumer.receive() == expected
                    metrics = os.pread(consumer.errors.fileno(), 65536, 0).decode()
                    reused = (cold[TARGET]["prompt_tokens"] - 1) // PAGE * PAGE
                    assert re.findall(r"\bremote_prefill_skipped_tokens=(\d+)\b", metrics) == ["20", "20"], metrics
                    assert re.findall(r"\breused=(\d+)\b", metrics) == ["0", str(reused)], metrics


def chain_and_tail(runs, directory, cold):
    # Each producer has space for one export. The directory root is removed
    # under pressure, while B holds the first four complete pages locally.
    with runs.worker("remote", directory, tenant="chain", capacity=1) as producer:
        ready = producer.infer(SOURCE, command="sync")
        assert ready["remote_published_pages"] == 1 and ready["remote_cache_evictions"] == 4, ready
        with runs.worker("remote", directory, tenant="chain", capacity=1) as gapped:
            gapped.infer("The quick br", command="sync")
            miss = gapped.infer(TARGET)
            match(miss, cold[TARGET], skip=0)
            assert miss["local"] == 12 and miss["remote_lookup_requests"] == 1, miss
        with runs.worker("remote", directory, tenant="chain", capacity=1) as consumer:
            warm = consumer.infer("The quick brown", command="sync")
            assert warm["remote_published_pages"] == 1 and warm["remote_cache_evictions"] == 3, warm
            partial = consumer.infer(TARGET)
            match(partial, cold[TARGET], skip=4)
            assert partial["local"] == 16 and partial["remote_lookup_requests"] == partial["data_requests"] == 1, partial
        with runs.worker("remote", directory, tenant="chain") as rootless:
            miss = rootless.infer(TARGET)
            match(miss, cold[TARGET], skip=0)
            assert miss["remote_lookup_requests"] == 1 and not miss["remote_lookup_hits"], miss

    with runs.worker("remote", directory, tenant="tail") as producer:
        short = producer.infer("a")
        assert short["prompt_tokens"] == 3 and short["remote_published_pages"] == 0, short
        with runs.worker("remote", directory, tenant="tail") as consumer:
            match(consumer.infer("a long suffix"), cold["a long suffix"], skip=0)

    aligned = SOURCE[:-1]
    with runs.worker("remote", directory, tenant="aligned") as producer:
        published = producer.infer(aligned)
        assert published["prompt_tokens"] == 20 and published["remote_published_pages"] == 5, published
        with runs.worker("remote", directory, tenant="aligned") as consumer:
            match(consumer.infer(aligned), cold[aligned], skip=16)


def faults(runs, directory, cold):
    listen = unused_port()
    with Proxy(listen) as proxy:
        opts = dict(tenant="faults", name="fault-source", listen=listen, advertise=proxy.endpoint)
        with runs.worker("remote", directory, **opts) as producer:
            producer.infer(SOURCE)
            for mode in ("drop", "wire-crc", "crc", "slow"):
                proxy.arm(mode)
                with runs.worker("remote", directory, tenant="faults", timeout=300 if mode == "slow" else 2000) as worker:
                    failed = worker.infer(TARGET)
                    match(failed, cold[TARGET], skip=0)
                    assert failed["remote_lookup_hits"] > 0 and failed["remote_prefetch_failures"] == 1, failed
                    assert failed["local"] == 0 and failed["remote_import_bytes"] == 0
                    if mode == "crc":
                        assert failed["remote_transfer_samples"] == 1, failed
                    if mode == "wire-crc":
                        assert failed["remote_transfer_samples"] == 0, failed
                assert proxy.injected == 1, mode

            # Each half can arrive within a separate 300 ms timeout, but both
            # halves together exceed that budget. Delay polling the engine
            # so the Connector must enforce the request deadline itself.
            proxy.arm("pace")
            with runs.worker("remote", directory, tenant="faults", timeout=300) as worker:
                deadline = worker.infer(TARGET, command="stall")
                match(deadline, cold[TARGET], skip=0)
                assert deadline["remote_prefetch_failures"] == 1 and deadline["page_transfer_ns"] == [], deadline
            assert proxy.seen == proxy.injected == 1, proxy.seen

            proxy.arm("slow")
            with runs.worker("remote", directory, tenant="faults", timeout=300) as worker:
                overlap = worker.infer(TARGET, "a", command="overlap")
                assert overlap["tokens"] == [cold[TARGET]["tokens"][0], cold["a"]["tokens"][0]], overlap
                assert overlap["remote_prefetch_failures"] == 1 and not overlap["remote_hit_pages"], overlap

            proxy.arm("slow")
            with runs.worker("remote", directory, tenant="faults") as worker:
                cancelled = worker.infer(TARGET, command="cancel")
                assert cancelled["remote_prefetch_cancelled"] == 1 and cancelled["prefilled"] == 0, cancelled
                assert cancelled["remote_prefill_skipped_tokens"] == 0
                proxy.arm("pass")
                resumed = worker.infer(TARGET)
                match(resumed, cold[TARGET], skip=20)
                assert resumed["local"] == 0, resumed

            if runs.args.device == "cuda":
                with runs.worker("remote", directory, tenant="faults") as worker:
                    cancelled = worker.infer(TARGET, command="cancel-upload")
                    assert cancelled["cuda_h2d_ns"] > 0 and cancelled["remote_prefetch_cancelled"] == 1, cancelled
                    assert cancelled["remote_hit_pages"] == cancelled["prefilled"] == 0, cancelled
                    resumed = worker.infer(TARGET, command="inspect")
                    match(resumed, cold[TARGET], skip=20)
                    assert resumed["local"] == 0 and resumed["kv_page_hashes"] == cold[TARGET]["kv_page_hashes"], resumed

            producer.close(check=False, kill=True)
            with runs.worker("remote", directory, tenant="faults") as worker:
                fallback = worker.infer(TARGET)
                match(fallback, cold[TARGET], skip=0)
                assert fallback["remote_prefill_fallbacks"] == 1, fallback

        with runs.worker("remote", directory, **opts) as restarted:
            restarted.infer(SOURCE)
            with runs.worker("remote", directory, tenant="faults") as worker:
                match(worker.infer(TARGET), cold[TARGET], skip=20)

    # The batch is served under valid tickets, then arrives after expiry.
    listen = unused_port()
    with runs.directory(ttl=250) as expiry_dir, Proxy(listen) as proxy:
        with runs.worker("remote", expiry_dir, listen=listen, advertise=proxy.endpoint) as producer:
            producer.infer(SOURCE)
            proxy.arm("expired")
            with runs.worker("remote", expiry_dir) as worker:
                expired = worker.infer(TARGET)
                match(expired, cold[TARGET], skip=0)
                assert expired["remote_lookup_hits"] and expired["remote_prefetch_failures"] == 1, expired
            assert proxy.injected == 1


def publication_retry(runs, directory, cold):
    with Proxy(int(directory.endpoint.split(":")[1])) as control:
        with runs.worker("remote", control, tenant="retry") as producer:
            control.arm("publish-drop")
            published = producer.infer(SOURCE)
            assert control.injected == 1
            assert published["remote_publish_retries"] >= 1 and published["remote_publish_failures"] == 0, published
            assert published["remote_published_pages"] == 5, published
            with runs.worker("remote", directory, tenant="retry") as consumer:
                match(consumer.infer(TARGET), cold[TARGET], skip=20)


def admission(runs, directory, cold):
    listen = unused_port()
    with Proxy(listen) as proxy:
        with runs.worker("remote", directory, tenant="admission", listen=listen, advertise=proxy.endpoint) as producer:
            producer.infer(SOURCE, OTHER)
            with runs.worker("remote", directory, tenant="admission") as consumer:
                proxy.arm("learn")
                match(consumer.infer(TARGET), cold[TARGET], skip=20)
                assert proxy.injected == 1
                proxy.arm("pass")
                skipped = consumer.infer(OTHER)
                match(skipped, cold[OTHER], skip=0)
                assert skipped["remote_admission_skips"] == 1 and skipped["data_requests"] == proxy.seen == 0, skipped
                assert skipped["remote_prefetch_failures"] == 0, skipped


def conversation(runs, directory):
    opts = dict(tenant="conversation", new=8, publish_generated=True)
    with runs.worker("cold", new=8) as oracle, contextlib.ExitStack() as workers:
        # Replay canonical token history as a chat service would. Re-tokenizing
        # text across an assistant/user boundary can change BPE segmentation.
        messages = [
            (SOURCE + " ") * 3 + "walked away",
            "What happened the next day?",
            "How did the story end?",
        ]
        history = []
        expected_reused = 0
        for turn, text in enumerate(messages, 1):
            encoded = oracle.infer(text, command="inspect")["prompt_token_ids"]
            history.extend(encoded if turn == 1 else encoded[1:])
            tokens = " ".join(map(str, history))
            expected = oracle.infer(tokens, command="inspect-ids")
            worker = workers.enter_context(runs.worker("remote", directory, **opts))
            result = worker.infer(tokens, command="inspect-ids")
            match(result, expected, skip=expected_reused)
            assert result["local"] == 0 and result["kv_page_hashes"] == expected["kv_page_hashes"], result
            if turn == 3:
                assert result["data_requests"] >= 2, result
            complete = (len(history) + len(result["tokens"][0]) - 1) // PAGE
            assert result["remote_published_pages"] == (complete - expected_reused // PAGE), result
            print(f"turn {turn}: prompt={len(history)} reused={expected_reused} "
                  f"published={result['remote_published_pages']} data_requests={result['data_requests']}")
            history.extend(result["tokens"][0])
            expected_reused = complete * PAGE


def contention(runs, directory, cold):
    with runs.worker("remote", directory, tenant="pressure") as producer:
        producer.infer(SOURCE, OTHER)
        with runs.worker("remote", directory, tenant="pressure", blocks=9, capacity=1) as consumer:
            result = consumer.infer(TARGET, OTHER, command="settle")
            assert result["tokens"] == [cold[TARGET]["tokens"][0], cold[OTHER]["tokens"][0]], result
            assert result["remote_prefetch_requests"] == 2 and result["remote_prefetch_failures"] >= 1, result
            assert result["remote_prefill_skipped_tokens"] > 0, result
        with runs.worker("remote", directory, tenant="eviction", blocks=9, capacity=48) as worker:
            base = worker.infer(SOURCE)
            assert base["remote_published_pages"] == 5
            evicted = worker.infer(OTHER)
            match(evicted, cold[OTHER], skip=0)
            assert evicted["remote_cache_evictions"] > 0, evicted


def integration(runs):
    with runs.worker("cold") as reference:
        cold = {TARGET: reference.infer(TARGET, command="inspect")}
        cold.update((prompt, reference.infer(prompt)) for prompt in (OTHER, "a", "a long suffix", SOURCE[:-1]))
    with runs.worker("local") as worker:
        worker.infer(SOURCE)
        result = worker.infer(TARGET)
        match(result, cold[TARGET], skip=0)
        assert result["local"] == 20, result
    with runs.directory() as directory:
        positive(runs, directory, cold)
        chain_and_tail(runs, directory, cold)
        faults(runs, directory, cold)
        publication_retry(runs, directory, cold)
        admission(runs, directory, cold)
        contention(runs, directory, cold)
        conversation(runs, directory)
    print(f"Nano-VLLM {runs.args.device}: independent workers matched cold outputs and exercised fallback")


def gpu_mib(pid):
    nvidia_smi = shutil.which("nvidia-smi")
    if not nvidia_smi:
        return None
    try:
        result = subprocess.run(
            [nvidia_smi, "--query-compute-apps=pid,used_memory", "--format=csv,noheader,nounits"],
            check=True, capture_output=True, text=True, timeout=8,
        )
        for row in csv.reader(result.stdout.splitlines()):
            if len(row) == 2 and row[0].strip() == str(pid):
                return int(row[1])
    except (ValueError, OSError, subprocess.SubprocessError):
        pass
    return None


def process_resources(pid):
    root = Path(f"/proc/{pid}")
    try:
        # The command name can itself contain spaces or parentheses.
        fields = (root / "stat").read_text().rpartition(")")[2].split()
        ticks = int(fields[11]) + int(fields[12])
        cpu_ns = ticks * 10**9 // os.sysconf("SC_CLK_TCK")
        rss = next(int(line.split()[1]) for line in (root / "status").read_text().splitlines()
                   if line.startswith("VmRSS:"))
        return cpu_ns, rss
    except (AttributeError, OSError, ValueError, IndexError, StopIteration):
        return None, None


def write_csv(output, rows):
    with output.open("w", encoding="utf-8", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)


def percentiles(values):
    order = sorted(values)
    return tuple(order[math.ceil(len(order) * q) - 1] for q in (0.50, 0.95, 0.99))


def benchmark(runs, output, samples, warmup=0, offset=0):
    warm = (SOURCE + " ") * 4 + "walks away"
    target = (SOURCE + " ") * 4 + "jumps high"
    rows = []
    transfers = []
    with runs.directory() as directory, runs.worker("remote", directory, new=8) as producer:
        publisher = producer.infer(warm)
        peers = {"publisher": producer.process.pid, "directory": directory.process.pid}
        expected = None
        for sample in range(offset, offset + samples):
            # Rotate measurement order to reduce drift across the three modes.
            modes = ("cold", "local", "remote")
            for mode in modes[sample % 3:] + modes[:sample % 3]:
                with runs.worker(mode, directory, new=8) as worker:
                    for _ in range(warmup):
                        worker.infer("A different story begins in a quiet valley.")
                    if mode == "local":
                        worker.infer(warm)
                    before = ({role: process_resources(pid)[0] for role, pid in peers.items()}
                              if mode == "remote" else {})
                    result = worker.infer(target)
                    resources = {}
                    for role, pid in peers.items():
                        after, rss = process_resources(pid) if mode == "remote" else (None, None)
                        first = before.get(role)
                        resources[f"{role}_cpu_ns"] = after - first if after is not None and first is not None else None
                        resources[f"{role}_rss_kib"] = rss
                    gpu = gpu_mib(worker.process.pid) if runs.args.device == "cuda" else None
                    resources["publisher_gpu_mib"] = (gpu_mib(producer.process.pid)
                                                      if runs.args.device == "cuda" and mode == "remote" else None)
                    resources["publisher_gpu_device_delta_bytes"] = (publisher["gpu_device_delta_bytes"]
                                                                     if mode == "remote" else None)
                tokens = result.pop("tokens")
                if expected is None:
                    expected = tokens
                assert tokens == expected, (mode, sample, tokens, expected)
                assert mode != "remote" or result["remote_prefill_skipped_tokens"] > 0, result
                assert mode != "local" or result["local"] > 0, result
                assert result["prefilled"] + result["local"] + result["remote_prefill_skipped_tokens"] == result["prompt_tokens"], result
                page_times = result.pop("page_transfer_ns")
                assert len(page_times) == result["remote_transfer_samples"], result
                assert sum(page_times) == result["remote_transfer_latency_ns"], result
                assert mode != "remote" or page_times, result
                rows.append(dict(
                    mode=mode, sample=sample + 1, device=runs.args.device, loopback="127.0.0.1",
                    generated_token_ids=" ".join(map(str, tokens[0])),
                    decode_tokens_per_s=(len(tokens[0]) - 1) * 1e9 / result["decode_ns"],
                    gpu_mib=gpu, **resources, **result,
                ))
                transfers.extend(dict(mode=mode, sample=sample + 1, device=runs.args.device,
                                      loopback="127.0.0.1", page_in_transfer=i, transfer_ns=duration)
                                 for i, duration in enumerate(page_times, 1))

    write_csv(output, rows)
    write_csv(output.with_name(output.stem + "-transfers.csv"), transfers)
    metrics = (
        "ttft_ns", "prefill_ns", "decode_tokens_per_s", "local", "remote_prefill_skipped_tokens",
        "cpu_ns", "rss_kib", "gpu_mib", "gpu_device_delta_bytes", "publisher_cpu_ns", "publisher_rss_kib",
        "publisher_gpu_mib", "publisher_gpu_device_delta_bytes", "directory_cpu_ns", "directory_rss_kib",
    )
    for mode in ("cold", "local", "remote"):
        for metric in metrics:
            values = [row[metric] for row in rows if row["mode"] == mode and row[metric] is not None]
            if values:
                p50, p95, p99 = percentiles(values)
                print(f"{mode} {metric} P50={p50:.3f} P95={p95:.3f} P99={p99:.3f}")
    p50, p95, p99 = percentiles(row["transfer_ns"] for row in transfers)
    print(f"remote transfer_ns P50={p50} P95={p95} P99={p99} pages={len(transfers)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--cli", type=Path, action="append", default=[])
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cpu")
    parser.add_argument("--benchmark", type=Path, help="write raw loopback samples to this CSV")
    parser.add_argument("--conversation", action="store_true", help="run three workers on canonical chat history")
    parser.add_argument("--samples", type=int, default=30)
    parser.add_argument("--warmup", type=int, default=0, help="independent warmup requests per consumer")
    parser.add_argument("--sample-offset", type=int, default=0, help="global sample offset for interleaved shards")
    parser.add_argument("--model", type=Path, help="optional checkpoint for benchmark mode")
    parser.add_argument("--tokenizer", type=Path, help="tokenizer for the optional checkpoint")
    args = parser.parse_args()
    if not __debug__:
        parser.error("run tests with assertions enabled")
    if (args.samples <= 0 or args.warmup < 0 or args.sample_offset < 0 or bool(args.model) != bool(args.tokenizer)
            or (args.model and not (args.benchmark or args.conversation))):
        parser.error("provide a positive sample count and pair --model/--tokenizer for benchmark or chat")
    try:
        with tempfile.TemporaryDirectory(prefix="nano-kv-") as tmp:
            runs = Runs(args, Path(tmp))
            if args.benchmark:
                benchmark(runs, args.benchmark, args.samples, args.warmup, args.sample_offset)
            elif args.conversation:
                with runs.directory() as directory:
                    conversation(runs, directory)
            else:
                integration(runs)
    except NoCuda:
        print("CUDA Runtime did not report an available device")
        return 77
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
