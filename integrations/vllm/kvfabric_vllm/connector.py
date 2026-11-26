"""vLLM 0.31.0 external KV cache. GPU blocks become visible after the entire load."""
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
import hashlib
import json
import logging
import math
import os
from pathlib import Path
import tempfile
import time
from threading import Lock

import nvtx
import torch
from vllm.distributed.kv_transfer.kv_connector.v1.base import (
    KVConnectorBase_V1,
    KVConnectorMetadata,
    KVConnectorRole,
    KVConnectorTransferResults,
)
from vllm.utils.gpu_sync_debug import gpu_sync_allowed
from vllm.v1.kv_cache_interface import FullAttentionSpec

from .bridge import Bridge, Spec
from .cuda_pages import CudaPages

logger = logging.getLogger(__name__)


@dataclass
class Transfer:
    request_id: str
    tokens: list[int]
    first: int
    blocks: list[int]


@dataclass
class Metadata(KVConnectorMetadata):
    loads: list[Transfer] = field(default_factory=list)
    saves: list[Transfer] = field(default_factory=list)


class KVFabricConnector(KVConnectorBase_V1):
    @property
    def requires_kv_delivery(self):
        return False

    @classmethod
    def get_required_kvcache_layout(cls, vllm_config):
        return "NHD"

    def __init__(self, vllm_config, role, kv_cache_config):
        super().__init__(vllm_config, role, kv_cache_config)
        parallel = vllm_config.parallel_config
        groups = kv_cache_config.kv_cache_groups
        if (
            parallel.tensor_parallel_size != 1 or parallel.pipeline_parallel_size != 1
            or parallel.decode_context_parallel_size != 1 or parallel.prefill_context_parallel_size != 1
            or len(groups) != 1 or type(groups[0].kv_cache_spec) is not FullAttentionSpec
        ):
            raise ValueError("KV-Fabric requires TP=PP=1 and one full-attention KV group")
        group = groups[0]
        spec = group.kv_cache_spec
        if spec.dtype != torch.bfloat16 or spec.sliding_window or spec.non_causal:
            raise ValueError("KV-Fabric requires causal BF16 KV")
        self._layers = tuple(group.layer_names)
        self._block = spec.block_size
        self._page_shape = (2, self._block, spec.num_kv_heads, spec.head_size)
        page_bytes = 2 * self._block * spec.num_kv_heads * spec.head_size * 2
        if page_bytes != spec.page_size_bytes or spec.head_size_v != spec.head_size:
            raise ValueError("KV-Fabric requires equal, unpadded K and V block geometry")

        extra = self._kv_transfer_config.kv_connector_extra_config
        self._lookup_ms = int(extra.get("lookup_timeout_ms", 300))
        self._load_ms = int(extra.get("load_timeout_ms", 10_000))
        self._verify = bool(extra.get("verify_gpu", False))
        self._metrics = extra.get("metrics_file")
        self._metrics_lock = Lock()
        disk_dir = extra.get("disk_dir")
        self._tier = "disk" if disk_dir else "cpu"
        model = vllm_config.model_config
        tokenizer_file = Path(model.tokenizer or model.model) / "tokenizer.json"
        model_key = json.dumps(
            [extra.get("checkpoint_id", model.model), model.revision, model.tokenizer,
             model.tokenizer_revision, model.hf_config.to_dict(),
             hashlib.sha256(tokenizer_file.read_bytes()).hexdigest() if tokenizer_file.is_file() else None,
             self._layers, self._page_shape],
            sort_keys=True, default=str,
        )
        # Keep source tiers homogeneous so a successfully committed page can be attributed.
        tenant = f"{extra.get('tenant', 'default')}:{self._tier}".encode()
        library = extra.get("library") or os.environ.get("KVF_VLLM_LIBRARY")
        if not library:
            raise ValueError("set KVF_VLLM_LIBRARY or connector extra_config.library")
        self._bridge = Bridge(
            library, Spec(extra.get("control", "127.0.0.1:9000").encode(), model_key.encode(),
                          tenant, self._block, len(self._layers), page_bytes)
        )
        if not self._bridge.window or min(self._load_ms, self._lookup_ms) < 1:
            raise ValueError("invalid KV-Fabric page geometry or timeout")

        self._executor = ThreadPoolExecutor(
            max_workers=2 if role == KVConnectorRole.SCHEDULER else 1, thread_name_prefix="kvfabric"
        )
        self._requests = {}
        self._lookups = {}
        self._failed_loads = set()
        self._cuda_mode = extra.get("cuda_batch")
        self._cuda = None
        self._scheduled_loads = []
        self._receiving = {}
        self._invalid = set()
        if role == KVConnectorRole.WORKER:
            disk_path = None
            if disk_dir:
                fd, disk_path = tempfile.mkstemp(prefix="kvfabric-", dir=disk_dir)
                os.close(fd)
            try:
                self._io_mode = self._bridge.open(
                    f"vllm-{os.getpid()}", os.fsencode(disk_path) if disk_path else None,
                    int(extra.get("cache_pages", 2048)), int(extra.get("io_depth", 4)),
                )
            finally:
                if disk_path:
                    os.unlink(disk_path)
            self._record("backend", io_mode=self._io_mode)

    def _record(self, event, **data):
        if self._metrics:
            with self._metrics_lock:
                with open(self._metrics, "a", encoding="utf-8") as stream:
                    stream.write(json.dumps({"event": event, "tier": self._tier, **data}) + "\n")

    def on_new_request(self, request):
        if (
            request.prompt_token_ids is not None and not request.mm_features
            and request.lora_request is None and request.cache_salt is None
        ):
            self._requests[request.request_id] = request

    def _lookup(self, tokens, first):
        _, hashes = self._bridge.chain(tokens)
        return self._bridge.lookup(hashes, first, len(tokens) // self._block - first, self._lookup_ms)

    def may_load_async(self, request):
        return (
            request.request_id in self._requests and request.request_id not in self._failed_loads
            and not request.skip_reading_prefix_cache
        )

    def get_num_new_matched_tokens(self, request, num_computed_tokens):
        request_id = request.request_id
        first = num_computed_tokens // self._block
        pages = (request.num_prompt_tokens - 1) // self._block
        if not self.may_load_async(request) or first >= pages:
            return 0, False
        prior = self._lookups.get(request_id)
        if prior is None or prior[0] != first:
            tokens = request.prompt_token_ids[:pages * self._block]
            self._lookups[request_id] = (first, self._executor.submit(self._lookup, tokens, first))
            return None, False
        if not prior[1].done():
            return None, False
        try:
            matched = prior[1].result()
        except Exception as exc:
            logger.exception("KV-Fabric prefix lookup failed")
            self._failed_loads.add(request_id)
            self._record("lookup_failure", request_id=request_id, status=str(exc))
            return 0, False
        available = max(0, matched * self._block - num_computed_tokens % self._block)
        return available, bool(available)

    def update_state_after_alloc(self, request, blocks, num_external_tokens):
        prior = self._lookups.pop(request.request_id, None)
        if num_external_tokens:
            first = prior[0] if prior is not None else request.num_computed_tokens // self._block
            end = (request.num_computed_tokens + num_external_tokens) // self._block
            self._scheduled_loads.append(Transfer(
                request.request_id, request.prompt_token_ids[:end * self._block],
                first, blocks.get_block_ids()[0][first:end],
            ))

    def build_connector_meta(self, scheduler_output):
        meta = Metadata(loads=self._scheduled_loads)
        self._scheduled_loads = []
        for request_id, count in scheduler_output.num_scheduled_tokens.items():
            request = self._requests.get(request_id)
            if request is None:
                continue
            first = request.num_computed_tokens // self._block
            end = min(request.num_prompt_tokens, request.num_computed_tokens + count) // self._block
            if end > first:
                blocks = self._kv_cache_manager.get_block_ids(request_id)[0][first:end]
                meta.saves.append(Transfer(
                    request_id, request.prompt_token_ids[:end * self._block], first, blocks
                ))
        return meta

    def update_connector_output(self, connector_output):
        # vLLM retries admission from position zero after an invalid load. Suppress
        # another external lookup for this request so it can recompute locally.
        self._failed_loads.update(connector_output.failed_recving & self._requests.keys())

    def request_finished(self, request, block_ids):
        self._failed_loads.discard(request.request_id)
        self._requests.pop(request.request_id, None)
        prior = self._lookups.pop(request.request_id, None)
        if prior is not None:
            prior[1].cancel()
        return False, None

    def register_kv_caches(self, kv_caches):
        self._tensors = []
        for name in self._layers:
            tensor = kv_caches[name]
            if tensor.ndim == 4 and tensor.shape[-1] == self._page_shape[-2] * self._page_shape[-1]:
                tensor = tensor.unflatten(-1, self._page_shape[-2:])
            if tensor.dtype != torch.bfloat16 or tensor.ndim != 5:
                raise ValueError(f"Unsupported KV tensor for {name}: {tensor.shape}, {tensor.dtype}")
            # FlashAttention and FlashInfer place the K/V axis on different dimensions.
            if tensor.shape[0] == 2 and tuple(tensor.shape[2:]) == self._page_shape[1:]:
                tensor = tensor.transpose(0, 1)
            if tuple(tensor.shape[1:]) != self._page_shape:
                raise ValueError(f"Unsupported NHD KV layout for {name}: {tensor.shape}")
            self._tensors.append(tensor)
        self._device = self._tensors[0].device
        self._load_stream = torch.cuda.Stream(device=self._device)
        cuda_available = hasattr(self._bridge.lib, "kvf_cuda_gather")
        if self._cuda_mode is True and not cuda_available:
            raise ValueError("cuda_batch requires a bridge built with MINIINFER_VLLM_CUDA")
        if self._cuda_mode is not False and cuda_available:
            pages = min(128, math.ceil(self._vllm_config.scheduler_config.max_num_batched_tokens / self._block))
            try:
                self._cuda = CudaPages(self._bridge.lib, self._tensors, self._page_shape, pages)
            except ValueError as error:
                if self._cuda_mode is True:
                    raise
                logger.info("KV-Fabric will use per-layer CUDA copies: %s", error)
        if self._cuda:
            self._load_stream.wait_stream(torch.cuda.current_stream(self._device))
            self._read_slot = self._cuda.slot()
            self._write_stream = torch.cuda.Stream(device=self._device)
            self._publisher = ThreadPoolExecutor(max_workers=1, thread_name_prefix="kvfabric-publish")
            self._write_slots = [(self._cuda.slot(), None) for _ in range(4)]
            return
        shape = (self._bridge.window, len(self._layers), *self._page_shape)
        self._read_stage = torch.empty(shape, dtype=torch.bfloat16, device="cpu", pin_memory=True)
        self._write_stage = torch.empty_like(self._read_stage, pin_memory=True)

    def start_load_kv(self, forward_context, **kwargs):
        for item in self._get_connector_metadata().loads:
            self._receiving[item.request_id] = (item, self._executor.submit(self._load, item))

    def _load(self, item):
        ids, hashes = self._bridge.chain(item.tokens)
        deadline = time.monotonic() + self._load_ms / 1000
        blocks_checked = 0
        stage = self._read_slot[0] if self._cuda else self._read_stage
        window = self._cuda.pages if self._cuda else self._bridge.window
        with gpu_sync_allowed(), torch.cuda.device(self._device), torch.cuda.stream(self._load_stream):
            for start in range(0, len(item.blocks), window):
                fetch_start = time.perf_counter()
                blocks = item.blocks[start:start + window]
                loaded = 0
                if self._cuda:
                    loaded = self._bridge.load_owned(
                        hashes, item.first + start, len(blocks), self._remaining_ms(deadline), stage.data_ptr()
                    )
                page_bytes = self._bridge.spec.layers * self._bridge.spec.layer_bytes
                for offset in range(loaded, len(blocks), self._bridge.window):
                    status = self._bridge.transfer(
                        ids, hashes, item.first + start + offset,
                        min(self._bridge.window, len(blocks) - offset),
                        stage.data_ptr() + offset * page_bytes, self._remaining_ms(deadline)
                    )
                    if status:
                        return self._bridge.describe(status), blocks_checked
                fetch_end = time.perf_counter()
                if self._cuda:
                    self._cuda.load(self._read_slot, blocks, self._load_stream)
                else:
                    with nvtx.annotate("kvfabric.h2d"):
                        for i, block in enumerate(blocks):
                            for layer, dst in enumerate(self._tensors):
                                dst[block].copy_(stage[i, layer], non_blocking=True)
                    with nvtx.annotate("kvfabric.sync.load"):
                        self._load_stream.synchronize()
                if os.environ.get("KVF_PERF_TRACE") == "1":
                    self._record("load_window", request_id=item.request_id, start=start,
                                 pages=len(blocks), payload_bytes=len(blocks) * page_bytes,
                                 fetch_ms=(fetch_end - fetch_start) * 1000,
                                 h2d_scatter_sync_ms=(time.perf_counter() - fetch_end) * 1000)
                if self._verify:
                    for i, block in enumerate(blocks):
                        for layer, dst in enumerate(self._tensors):
                            if not torch.equal(dst[block].cpu(), stage[i, layer]):
                                return "GPU block mismatch", blocks_checked
                            blocks_checked += 1
                if time.monotonic() >= deadline:
                    return "timeout", blocks_checked
        return None, blocks_checked

    @staticmethod
    def _remaining_ms(deadline):
        return max(0, math.ceil((deadline - time.monotonic()) * 1000))

    def wait_for_layer_load(self, layer_name):
        # Async results are handed to the scheduler only after the full GPU copy completes.
        return

    def save_kv_layer(self, layer_name, kv_layer, attn_metadata, **kwargs):
        # Capture happens after forward, while the scheduler still owns the source blocks.
        return

    def _publish_snapshot(self, item, first, count, stage, ready):
        try:
            with nvtx.annotate("kvfabric.sync.save.background"), gpu_sync_allowed():
                ready.synchronize()
            ids, hashes = self._bridge.chain(item.tokens)
            page_bytes = self._bridge.spec.layers * self._bridge.spec.layer_bytes
            for offset in range(0, count, self._bridge.window):
                pages = min(count - offset, self._bridge.window)
                with nvtx.annotate("kvfabric.publish.background"):
                    status = self._bridge.transfer(
                        ids, hashes, first + offset, pages, stage.data_ptr() + offset * page_bytes
                    )
                if status:
                    self._record("save_failure", request_id=item.request_id, status=self._bridge.describe(status))
                    return
                self._record("stored", request_id=item.request_id, tokens=pages * self._block)
        except Exception as exc:
            logger.exception("KV-Fabric background publication failed")
            self._record("save_failure", request_id=item.request_id, status=str(exc))
        finally:
            self._record("save_complete", request_id=item.request_id, first=first,
                         tokens=count * self._block)

    def wait_for_save(self):
        saves = self._get_connector_metadata().saves
        if self._cuda:
            with gpu_sync_allowed():
                stream = torch.cuda.current_stream(self._device)
                for item in saves:
                    for start in range(0, len(item.blocks), self._cuda.pages):
                        slot_index = next((i for i, (_, done) in enumerate(self._write_slots)
                                           if done is None or done.done()), None)
                        if slot_index is None:
                            self._record("save_skipped", request_id=item.request_id, reason="staging busy")
                            break
                        slot, done = self._write_slots[slot_index]
                        if done is not None:
                            try:
                                done.result()
                            except Exception as exc:
                                logger.exception("KV-Fabric background save failed")
                                self._record("save_failure", request_id=item.request_id, status=str(exc))
                        blocks = item.blocks[start:start + self._cuda.pages]
                        ready = self._cuda.snapshot(slot, blocks, stream, self._write_stream)
                        self._record("save_queued", request_id=item.request_id, first=item.first + start,
                                     tokens=len(blocks) * self._block)
                        future = self._publisher.submit(
                            self._publish_snapshot, item, item.first + start, len(blocks), slot[0], ready
                        )
                        self._write_slots[slot_index] = (slot, future)
            return
        stage = self._write_stage
        with gpu_sync_allowed():
            for item in saves:
                ids, hashes = self._bridge.chain(item.tokens)
                for start in range(0, len(item.blocks), self._bridge.window):
                    blocks = item.blocks[start:start + self._bridge.window]
                    with nvtx.annotate("kvfabric.d2h"):
                        for i, block in enumerate(blocks):
                            for layer, src in enumerate(self._tensors):
                                stage[i, layer].copy_(src[block], non_blocking=True)
                    with nvtx.annotate("kvfabric.sync.save"):
                        torch.cuda.current_stream(self._device).synchronize()
                    status = self._bridge.transfer(ids, hashes, item.first + start, len(blocks),
                                                   stage.data_ptr())
                    if status:
                        self._record("save_failure", request_id=item.request_id, status=self._bridge.describe(status))
                        logger.warning("KV-Fabric save failed: %s", self._bridge.describe(status))
                        break
                    self._record("stored", request_id=item.request_id, tokens=len(blocks) * self._block)

    def get_transfer_results(self, finished_req_ids):
        results = KVConnectorTransferResults()
        for request_id, (item, future) in tuple(self._receiving.items()):
            if not future.done():
                continue
            del self._receiving[request_id]
            try:
                error, blocks_checked = future.result()
            except Exception as exc:
                logger.exception("KV-Fabric GPU copy failed")
                error, blocks_checked = str(exc), 0
            results.finished_recving.add(request_id)
            if error:
                self._invalid.update(item.blocks)
                results.failed_recving.add(request_id)
                self._record("load_failure", request_id=request_id, status=error)
            else:
                self._record("hit", request_id=request_id, tokens=len(item.blocks) * self._block,
                             blocks_checked=blocks_checked)
        return results

    def get_block_ids_with_load_errors(self):
        invalid, self._invalid = self._invalid, set()
        return invalid

    def shutdown(self):
        self._executor.shutdown(wait=True, cancel_futures=True)
        if self._cuda:
            self._publisher.shutdown(wait=True)
        self._bridge.close()
