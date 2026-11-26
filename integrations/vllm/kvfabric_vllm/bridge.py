"""ctypes bindings for the bounded CPU staging ABI."""
import ctypes as C

Hash = C.c_ubyte * 32


class Spec(C.Structure):
    _fields_ = [
        ("control", C.c_char_p),
        ("model_key", C.c_char_p),
        ("tenant", C.c_char_p),
        ("block_tokens", C.c_uint32),
        ("layers", C.c_uint32),
        ("layer_bytes", C.c_uint32),
    ]


class Bridge:
    def __init__(self, library, spec):
        if not spec.block_tokens or not spec.layers or not spec.layer_bytes:
            raise ValueError("invalid KV-Fabric page geometry")
        self.spec = spec
        self.worker = C.c_void_p()
        self.lib = C.CDLL(library)
        for name, result, arguments in (
            ("hashes", C.c_int, [C.POINTER(Spec), C.c_void_p, C.c_size_t, C.c_void_p]),
            ("lookup", C.c_int, [C.POINTER(Spec), C.c_void_p, C.c_size_t, C.c_uint32, C.POINTER(C.c_size_t)]),
            ("open", C.c_int, [C.POINTER(Spec), C.c_char_p, C.c_char_p, C.c_size_t, C.c_uint32,
                               C.POINTER(C.c_void_p)]),
            ("close", None, [C.c_void_p]),
            ("io_mode", C.c_char_p, [C.c_void_p]),
            ("publish", C.c_int, [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, C.c_void_p]),
            ("load_owned", C.c_size_t, [C.c_void_p, C.c_void_p, C.c_size_t, C.c_uint32, C.c_void_p]),
            ("load", C.c_int, [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, C.c_uint32, C.c_void_p]),
        ):
            function = getattr(self.lib, f"kvf_vllm_{name}", None)
            if function is None and name == "load_owned":
                continue
            function.argtypes = arguments
            function.restype = result
        self.lib.kvf_status_string.argtypes = [C.c_int]
        self.lib.kvf_status_string.restype = C.c_char_p
        if hasattr(self.lib, "kvf_vllm_open_remote"):
            self.lib.kvf_vllm_open_remote.argtypes = [C.POINTER(Spec), C.c_char_p, C.c_char_p,
                C.c_size_t, C.c_uint32, C.c_char_p, C.c_char_p, C.c_uint32, C.POINTER(C.c_void_p)]
            self.lib.kvf_vllm_open_remote.restype = C.c_int
        if hasattr(self.lib, "kvf_vllm_load_direct"):
            self.lib.kvf_vllm_load_direct.argtypes = self.lib.kvf_vllm_load.argtypes
            self.lib.kvf_vllm_load_direct.restype = C.c_int
        self.window = min(8, (8 * 1024 * 1024) // (spec.layers * spec.layer_bytes))

    def describe(self, status):
        return self.lib.kvf_status_string(status).decode()

    def open(self, name, disk_path, pages, depth, listen=None, transport=None, timeout_ms=0):
        if hasattr(self.lib, "kvf_vllm_open_remote"):
            status = self.lib.kvf_vllm_open_remote(
                C.byref(self.spec), name.encode(), disk_path, pages, depth,
                listen.encode() if listen else None, transport.encode() if transport else None,
                timeout_ms, C.byref(self.worker)
            )
        else:
            if listen or transport:
                raise RuntimeError("library lacks explicit remote transport ABI")
            status = self.lib.kvf_vllm_open(C.byref(self.spec), name.encode(), disk_path,
                                          pages, depth, C.byref(self.worker))
        if status:
            raise RuntimeError(f"KV-Fabric bridge open: {self.describe(status)}")
        return self.lib.kvf_vllm_io_mode(self.worker).decode()

    def pending_closes(self):
        function = getattr(self.lib, "kvf_vllm_pending_closes", None)
        return int(function()) if function else 0

    def close(self):
        self.lib.kvf_vllm_close(self.worker)
        self.worker = C.c_void_p()

    def chain(self, tokens):
        assert len(tokens) % self.spec.block_tokens == 0
        ids = (C.c_int32 * len(tokens))(*tokens)
        # Slot zero is the zero parent; slot i+1 names page i.
        hashes = (Hash * (len(tokens) // self.spec.block_tokens + 1))()
        status = self.lib.kvf_vllm_hashes(
            C.byref(self.spec), ids, len(hashes) - 1, C.byref(hashes, C.sizeof(Hash))
        )
        if status:
            raise RuntimeError(f"KV-Fabric hash: {self.describe(status)}")
        return ids, hashes

    def lookup(self, hashes, first, pages, timeout_ms):
        matched = C.c_size_t()
        status = self.lib.kvf_vllm_lookup(
            C.byref(self.spec), C.byref(hashes, (first + 1) * C.sizeof(Hash)),
            pages, timeout_ms, C.byref(matched)
        )
        return matched.value if not status else 0

    def load_owned(self, hashes, first, pages, timeout_ms, pointer):
        return self.lib.kvf_vllm_load_owned(
            self.worker, C.byref(hashes, (first + 1) * C.sizeof(Hash)), pages, timeout_ms, pointer
        )

    def transfer(self, ids, hashes, first, pages, pointer, timeout_ms=None, direct=False):
        args = (
            self.worker, hashes[first],
            C.byref(ids, first * self.spec.block_tokens * C.sizeof(C.c_int32)), pages,
        )
        if timeout_ms is None:
            return self.lib.kvf_vllm_publish(*args, pointer)
        fn = self.lib.kvf_vllm_load_direct if direct else self.lib.kvf_vllm_load
        return fn(*args, timeout_ms, pointer)
