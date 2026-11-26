"""Connector method regressions without claiming a real vLLM scheduler run."""
import ast
from concurrent.futures import Future, ThreadPoolExecutor
from contextlib import nullcontext
from dataclasses import dataclass, field
import json
import logging
from pathlib import Path
from threading import Lock
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[2]

@pytest.fixture
def connector():
    # Compile the actual method bodies with minimal base classes. vLLM/nvtx are
    # optional dependencies; these tests deliberately do not emulate inference.
    path = ROOT / "integrations/vllm/kvfabric_vllm/connector.py"
    tree = ast.parse(path.read_text())
    tree.body = [item for item in tree.body if isinstance(item, (ast.ClassDef, ast.FunctionDef))]
    ns = dict(dataclass=dataclass, field=field, json=json, Lock=Lock,
              logger=logging.getLogger("audit"), KVConnectorBase_V1=object,
              KVConnectorMetadata=object, gpu_sync_allowed=nullcontext,
              nvtx=SimpleNamespace(annotate=lambda *a: nullcontext()),
              KVConnectorTransferResults=lambda: SimpleNamespace(finished_recving=set(), failed_recving=set()))
    exec(compile(tree, str(path), "exec"), ns)
    cls = ns["KVFabricConnector"]
    obj = cls.__new__(cls)
    obj._block = 16
    obj._requests = {}
    obj._failed_loads = set()
    obj._lookups = {}
    obj._metrics = None
    obj._metrics_lock = Lock()
    obj._tier = "cpu"
    obj._scheduled_loads = []
    obj._receiving = {}
    obj._invalid = set()
    return obj

def test_metrics_concurrent(connector, tmp_path):
    connector._metrics = tmp_path / "metrics.jsonl"
    def record(lane):
        for i in range(1000):
            connector._record("parallel", lane=lane, index=i)
    with ThreadPoolExecutor(4) as pool:
        list(pool.map(record, range(4)))
    lines = [json.loads(line) for line in connector._metrics.read_text().splitlines()]
    assert len(lines) == 4000
    assert len({(x["lane"], x["index"]) for x in lines}) == 4000

@pytest.mark.parametrize("computed,matched,expected", [(5,2,27),(16,2,32),(5,0,0),(31,1,1)])
def test_partial_blocks(connector, computed, matched, expected):
    request = SimpleNamespace(request_id="r", num_prompt_tokens=100, prompt_token_ids=list(range(100)),
                              skip_reading_prefix_cache=False, num_computed_tokens=computed)
    connector._requests["r"] = request
    f = Future();f.set_result(matched)
    connector._lookups["r"] = (computed//16, f)
    assert connector.get_num_new_matched_tokens(request, computed) == (expected, bool(expected))
    if expected:
        blocks = SimpleNamespace(get_block_ids=lambda: [list(range(7))])
        connector.update_state_after_alloc(request, blocks, expected)
        item = connector._scheduled_loads[0]
        assert item.first == computed//16
        assert len(item.blocks) == matched

@pytest.mark.parametrize("error", [ValueError("lookup"), RuntimeError("lookup")])
def test_lookup_future_exception(connector, error):
    request = SimpleNamespace(request_id="r", num_prompt_tokens=64, skip_reading_prefix_cache=False)
    connector._requests["r"] = request
    f=Future();f.set_exception(error);connector._lookups["r"]=(0,f)
    assert connector.get_num_new_matched_tokens(request,0)==(0,False)
    assert "r" in connector._failed_loads

def test_missing_prior(connector):
    request=SimpleNamespace(request_id="r", num_computed_tokens=5, prompt_token_ids=list(range(64)))
    connector.update_state_after_alloc(request,SimpleNamespace(get_block_ids=lambda:[[10,11,12,13]]),27)
    assert connector._scheduled_loads[0].blocks==[10,11]

def test_receive_future_exception(connector):
    f=Future();f.set_exception(ValueError("receive"))
    connector._receiving["r"]=(SimpleNamespace(blocks=[3,4]),f)
    results=connector.get_transfer_results(set())
    assert results.failed_recving==results.finished_recving=={"r"}
    assert connector.get_block_ids_with_load_errors()=={3,4}

def test_background_publish_exception_reported(connector):
    def broken(*args): raise ValueError("publish")
    connector._bridge=SimpleNamespace(chain=broken)
    events=[];connector._record=lambda event,**data: events.append((event,data))
    item=SimpleNamespace(request_id="r",tokens=[1,2])
    connector._publish_snapshot(item,0,1,None,SimpleNamespace(synchronize=lambda:None))
    assert [e for e,_ in events]==["save_failure","save_complete"]
