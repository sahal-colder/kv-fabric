#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 4 ]]; then
  echo "usage: $0 {baseline|optimized|bench} BUILD_DIR MODEL_DIR OUTPUT_DIR" >&2
  exit 2
fi
mode=$1
build=$(realpath "$2")
model=$3
out=$(realpath -m "$4")
repo=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$out"
export PYTHONPATH="$repo/scripts${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONDONTWRITEBYTECODE=1 VLLM_NO_USAGE_STATS=1

if [[ $mode == bench ]]; then
  bench="$build/bin/kvf_cuda_bench"
  ncu=${NCU:-ncu}
  "$bench" | tee "$out/cuda-bench.csv"
  status=0
  for kernel in gather scatter; do
    report="$out/$kernel"
    if "$ncu" --kernel-name "regex:${kernel}_kv_pages" --launch-count 1 \
        --section SpeedOfLight --section Occupancy \
        --section MemoryWorkloadAnalysis --section MemoryWorkloadAnalysis_Tables \
        --clock-control base \
        --metrics dram__bytes.sum.per_second,dram__bytes_read.sum,dram__bytes_write.sum,dram__cycles_elapsed.avg.per_second,gpu__time_duration.sum,l1tex__t_sectors_pipe_lsu_mem_global_op_st.sum \
        --force-overwrite -o "$report" "$bench" vector >"$report.log" 2>&1; then
      if [[ "$report.ncu-repz" -nt "$report.ncu-rep" ]]; then
        report="$report.ncu-repz"
      else
        report="$report.ncu-rep"
      fi
      if ! "$ncu" --import "$report" --page raw --csv > "$out/$kernel.csv"; then
        status=1
      fi
    else
      cat "$out/$kernel.log" >&2
      status=1
    fi
  done
  exit "$status"
fi
if [[ $mode != baseline && $mode != optimized ]]; then
  echo "unknown profiling mode: $mode" >&2
  exit 2
fi
trace="$out/$mode"
driver=(python3 "$repo/scripts/profile_vllm_kv.py" "$mode" "$build" "$model" "$out")
if [[ ${KVF_PROFILE:-1} == 0 ]]; then
  "${driver[@]}"
  exit
fi
"${NSYS:-nsys}" profile --trace=cuda,nvtx,osrt --sample=none --cpuctxsw=none \
  --capture-range=cudaProfilerApi --capture-range-end=stop \
  --trace-fork-before-exec=true --cuda-graph-trace=node --cuda-flush-interval=0 --osrt-threshold=10000 \
  -o "$trace" "${driver[@]}" --profile
"${NSYS:-nsys}" export --type=sqlite --output "$trace.sqlite" "$trace.nsys-rep"
python3 "$repo/scripts/summarize_nsys.py" "$trace.sqlite" "$out/requests.json" --out "$out/summary.json"
