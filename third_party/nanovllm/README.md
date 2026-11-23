# Sources and licenses

This project is an independent educational C implementation with its own CUDA runtime.

## karpathy/llama2.c

- Repository: https://github.com/karpathy/llama2.c
- Reference revision: `350e04fe35433e6d2941dce5a1f53308f87058eb`
- Source: [run.c](https://github.com/karpathy/llama2.c/blob/350e04fe35433e6d2941dce5a1f53308f87058eb/run.c)
- License: MIT, Copyright (c) 2023 Andrej; [full notice](llama2.c.LICENSE).
- Adapted in `src/nanovllm/model.c`: checkpoint tensor order, RMSNorm, RoPE, attention,
  SwiGLU and forward computation. KV storage changed to physical pages; external
  checkpoint lengths and dimensions are checked before tensor mapping.
- Adapted in `src/nanovllm/tokenizer.c`: BPE merge algorithm, dummy prefix, UTF-8 byte
  fallback and byte-token decoding; the loader validates file entries.
- `src/nanovllm/sampler.c` independently expresses greedy/top-p sampling and uses the
  same xorshift64* random generator family, with a state per request.
- The optional comparison script downloads the reference into a temporary
  directory and wraps its actual forward/tokenizer code for token comparison.

## GeeeekExplorer/nano-vllm

- Repository: https://github.com/GeeeekExplorer/nano-vllm
- Reference revision: `bb823b3e06983d71485a8e1f23715ebd87d98ef8`
- Sources: [block_manager.py](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/engine/block_manager.py),
  [scheduler.py](https://github.com/GeeeekExplorer/nano-vllm/blob/bb823b3e06983d71485a8e1f23715ebd87d98ef8/nanovllm/engine/scheduler.py).
- License: MIT, Copyright (c) 2025 Xingkai Yu; [full notice](nano-vllm.LICENSE).
- `src/nanovllm/cache.c` and `src/nanovllm/engine.c` adapt the architecture of block tables,
  reference counts, completed-prefix reuse and recompute preemption to C.
- This version uses two rolling hashes and the prefix length to index cached
  blocks. An indexed LRU tracks idle blocks, and hierarchical bitsets find the
  oldest waiting or running request.

## Example pretrained weights and tokenizers

- [karpathy/tinyllamas](https://huggingface.co/karpathy/tinyllamas) (MIT model
  repository), revision `0bd21da7698eaf29a0d7de3992de8a46ef624add`.
- `stories15M.bin` and `stories260K/stories260K.bin` are pretrained TinyStories
  models published by Andrej Karpathy. The latter uses GQA and a 512-token vocab.
- `stories260K/tok512.bin` comes from that model repository; the 32000-token
  `tokenizer.bin` comes from the pinned llama2.c revision above.
- Downloads are opt-in and kept in ignored `models/`; their exact SHA256
  checksums are listed in `tools/download.py` and verified before installation.
- These model files have their own provenance and licensing; the new project
  code's top-level MIT license does not replace their original notices.

Sources were consulted on 2026-11-04. No generated SDK or Protocol files are used.
