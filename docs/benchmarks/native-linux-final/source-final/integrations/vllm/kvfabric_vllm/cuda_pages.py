"""Bounded pinned/device staging and the vectorized KV copy entry points."""
import ctypes as C

import nvtx
import torch


class CudaPages:
    def __init__(self, lib, tensors, shape, pages):
        self.device = tensors[0].device
        self.lib = lib
        self.pages = pages
        self.shape = (pages, len(tensors), *shape)
        self.tokens, self.heads, head_dim = shape[1:]
        self.head_bytes = head_dim * tensors[0].element_size()
        if self.head_bytes % 16:
            raise ValueError("KV-Fabric CUDA packing requires aligned KV head rows")
        descriptors = []
        for tensor in tensors:
            stride = [s * tensor.element_size() for s in tensor.stride()]
            if (tensor.device != self.device or tensor.data_ptr() % 16
                or any(s % 16 for s in stride[:-1]) or tensor.stride(-1) != 1):
                raise ValueError(f"Unsupported KV strides for 16-byte packing: {tensor.stride()}")
            descriptors.append((tensor.data_ptr(), *stride[:-1]))
        self.layers = torch.tensor(descriptors, dtype=torch.int64, device=self.device)
        for name in ("kvf_cuda_gather", "kvf_cuda_scatter"):
            function = getattr(lib, name)
            function.argtypes = [C.c_void_p, C.c_void_p, C.c_uint, C.c_uint, C.c_uint, C.c_uint, C.c_uint,
                                 C.c_void_p, C.c_void_p]
            function.restype = C.c_int

    def slot(self):
        host = torch.empty(self.shape, dtype=torch.bfloat16, pin_memory=True)
        device = torch.empty(self.shape, dtype=torch.bfloat16, device=self.device)
        block_host = torch.empty(self.pages, dtype=torch.int32, pin_memory=True)
        block_device = torch.empty(self.pages, dtype=torch.int32, device=self.device)
        return host, device, block_host, block_device

    def launch(self, slot, blocks, stream, gather):
        _, data, block_host, block_device = slot
        count = len(blocks)
        block_host[:count].copy_(torch.as_tensor(blocks, dtype=torch.int32, device="cpu"))
        block_device[:count].copy_(block_host[:count], non_blocking=True)
        function = self.lib.kvf_cuda_gather if gather else self.lib.kvf_cuda_scatter
        status = function(self.layers.data_ptr(), block_device.data_ptr(), count, self.shape[1],
                          self.tokens, self.heads, self.head_bytes, data.data_ptr(), stream.cuda_stream)
        if status:
            raise RuntimeError(f"KV-Fabric CUDA {function.__name__} failed with status {status}")

    def load(self, slot, blocks, stream):
        host, device, _, _ = slot
        with nvtx.annotate("kvfabric.h2d"):
            device[:len(blocks)].copy_(host[:len(blocks)], non_blocking=True)
        with nvtx.annotate("kvfabric.scatter"):
            self.launch(slot, blocks, stream, gather=False)
        with nvtx.annotate("kvfabric.sync.load"):
            stream.synchronize()

    def snapshot(self, slot, blocks, stream, copy_stream):
        host, device, _, _ = slot
        with nvtx.annotate("kvfabric.gather"):
            self.launch(slot, blocks, stream, gather=True)
        # The gather is queued on the model stream before that stream can reuse
        # any source page. The copy thread only touches private staging.
        ready = stream.record_event()
        with torch.cuda.stream(copy_stream), nvtx.annotate("kvfabric.d2h"):
            copy_stream.wait_event(ready)
            host[:len(blocks)].copy_(device[:len(blocks)], non_blocking=True)
            return copy_stream.record_event()

