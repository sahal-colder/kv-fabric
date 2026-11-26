#!/usr/bin/env bash
# Full Before/After reproduction on the documented SIW namespace fallback.
# Usage: bash scripts/reproduce_rdma_cross_instance.sh MODEL BUILD OUT [PYTHON]
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
[[ $# == 3 || $# == 4 ]] || { head -3 "$0"; exit 2; }
model=$(realpath "$1")
build=$(realpath "$2")
out=$(realpath -m "$3")
python_bin=${4:-$(command -v python3)}
[[ ! -e $out ]] || { echo "Use a new output directory" >&2; exit 1; }
mkdir -p "$out"
python3 "$repo/scripts/build_siw_netns_testbed.py" --out "$out/siw-module"
python3 "$repo/scripts/build_rdma_baseline.py" "$out/before-source" "$out/before-build" > "$out/before-build.log" 2>&1
common=(--provider siw --provider-module "$out/siw-module/siw.ko" --python "$python_bin" --model "$model" --rounds 15)
# Each privileged invocation installs its own EXIT trap and restores state.
sudo bash "$repo/scripts/bench_rdma_cross_instance.sh" "${common[@]}" \
  --phase reference --build "$out/before-build" --out "$out/reference"
sudo bash "$repo/scripts/bench_rdma_cross_instance.sh" "${common[@]}" \
  --phase replay --build "$build" --out "$out/after" \
  --reference "$out/reference/cold/requests.jsonl" --snapshot-from "$out/reference/snapshot" \
  --buffer-bytes 1048576 --queue-size 384 --inline 64 --chunk-pages 128
sudo bash "$repo/scripts/bench_rdma_cross_instance.sh" "${common[@]}" \
  --phase tune --build "$build" --out "$out/tuning" --snapshot-from "$out/reference/snapshot"
python3 "$repo/scripts/summarize_rdma_cross_instance.py" --reference-dir "$out/reference" \
  --ablation-dir "$out/after" --after-dir "$out/after" --ceiling-dir "$out/reference" \
  --tuning-dir "$out/tuning" --out "$out/report"
