#!/usr/bin/env python3
"""Run the canonical bundled DOCX renderer with this host's private ABI loader."""
import importlib.util
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "docs/benchmarks/native-linux-final/document-runtime-compat.json"
RENDERER = Path(os.environ.get("KVF_DOCX_RENDERER", str(Path.home() /
    ".codex/plugins/cache/openai-primary-runtime/documents/26.1123.11515/skills/documents/render_docx.py")))
spec = importlib.util.spec_from_file_location("canonical_docx_renderer", RENDERER)
renderer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(renderer)
if CONFIG.exists():
    config = json.loads(CONFIG.read_text())
    wrappers = Path(config["wrapper_directory"])
    os.environ["PATH"] = str(wrappers) + os.pathsep + os.environ["PATH"]
    # Only executable resolution changes. Conversion, isolated profiles and PNG
    # rasterization still run in the canonical renderer, using bundled binaries.
    renderer._resolve_soffice = lambda: str(wrappers / "soffice")
renderer.main()
