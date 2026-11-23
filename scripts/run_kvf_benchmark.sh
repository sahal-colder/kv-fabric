#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

if [[ $# != 4 ]]; then
  echo "usage: $0 KVFABRICD MINIINFER_WORKER KVF_BENCH OUTPUT_DIRECTORY" >&2
  exit 2
fi
dir_bin=$(realpath "$1")
worker_bin=$(realpath "$2")
bench_bin=$(realpath "$3")
output=$(realpath -m "$4")
scratch=${KVF_SSD_ROOT:-/tmp/kvfabric-ssd}
devices=${KVF_BENCH_DEVICES-cpu}
mkdir -p -- "$scratch" "$output"
filesystem=$(findmnt -n -o FSTYPE --target "$scratch")
if [[ $filesystem != ext4 ]]; then
  printf 'Benchmark requires Linux ext4. Found %s at %s\n' "$filesystem" "$scratch" >&2
  exit 2
fi
work=$(mktemp -d "$scratch/run.XXXXXX")
worker_pid=
directory_pid=
status=1
cleanup() {
  if [[ -n "$worker_pid" ]]; then
    kill "$worker_pid" 2>/dev/null || true
    wait "$worker_pid" 2>/dev/null || true
  fi
  if [[ -n "$directory_pid" ]]; then
    kill "$directory_pid" 2>/dev/null || true
    wait "$directory_pid" 2>/dev/null || true
  fi
  if (( status == 0 )); then
    rm -f -- "$work"/{config,namespace,tokens,tensors,worker.log,directory.log,tcp-raw.csv,tcp-summary.csv,ssd-raw.csv,ssd-summary.csv}
    rmdir "$work"
  else
    printf 'benchmark scratch retained at %s\n' "$work" >&2
  fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

printf 'benchmark fs=%s source=%s kernel=%s\n' "$filesystem" "$(findmnt -n -o SOURCE --target "$work")" "$(uname -r)"
selected=()
if [[ -n $devices ]]; then
  read -r -a selected <<< "$devices"
fi
sizes=(4096 65536 262144 1048576 4194304)
concurrencies=(1 4 16 32)

ready() {
  local pid=$1 log=$2 line i
  for ((i = 0; i < 200; ++i)); do
    if IFS= read -r line < "$log" && [[ $line == "READY "* ]]; then
      printf '%s\n' "$line"
      return
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      cat "$log" >&2
      return 1
    fi
    sleep 0.05
  done
  cat "$log" >&2
  return 1
}

csv_headers() {
  printf '%s\n' 'mode,device,layer_bytes,concurrency,round,client,sample,latency_ns' > "$work/$1-raw.csv"
  printf '%s\n' 'mode,device,layer_bytes,concurrency,round,samples,wall_ns,throughput_mib_s,p50_us,p95_us,p99_us,client_cpu_ms,directory_cpu_ms,source_cpu_ms,client_peak_rss_kib,directory_peak_rss_kib,source_peak_rss_kib' > "$work/$1-summary.csv"
}

if [[ -n $devices ]]; then
  printf '%s\n' 'listen = 127.0.0.1:0' 'lanes = 64' > "$work/config"
  "$dir_bin" --config "$work/config" > "$work/directory.log" 2>&1 &
  directory_pid=$!
  read -r _ control <<< "$(ready "$directory_pid" "$work/directory.log")"
  csv_headers tcp
  for device in "${selected[@]}"; do
    for bytes in "${sizes[@]}"; do
      "$bench_bin" fixture "$bytes" "$work"
      : > "$work/worker.log"
      MINIINFER_WORKER_LANES=32 "$worker_bin" serve bench-source "$control" 127.0.0.1:0 "$device" \
        "$work/namespace" "$work/tokens" "$work/tensors" > "$work/worker.log" 2>&1 &
      worker_pid=$!
      ready "$worker_pid" "$work/worker.log" > /dev/null
      for concurrency in "${concurrencies[@]}"; do
        for mode in short pooled; do
          "$bench_bin" run "$mode" "$device" "$control" "$bytes" "$concurrency" "$directory_pid" "$worker_pid" \
            "$work/tcp-raw.csv" "$work/tcp-summary.csv"
        done
        printf 'TCP %s %d bytes concurrency=%d\n' "$device" "$bytes" "$concurrency"
      done
      kill "$worker_pid"
      wait "$worker_pid"
      worker_pid=
    done
  done
  cp -- "$work/tcp-raw.csv" "$output/kvf-ultimate-tcp-raw.csv"
  cp -- "$work/tcp-summary.csv" "$output/kvf-ultimate-tcp-summary.csv"
fi

if [[ ${KVF_BENCH_SSD:-1} == 1 ]]; then
  csv_headers ssd
  for bytes in "${sizes[@]}"; do
    for concurrency in "${concurrencies[@]}"; do
      for mode in buffered direct; do
        "$bench_bin" ssd "$mode" "$work" "$bytes" "$concurrency" "$work/ssd-raw.csv" "$work/ssd-summary.csv"
      done
      printf 'SSD %d bytes concurrency=%d\n' "$bytes" "$concurrency"
    done
  done
  cp -- "$work/ssd-raw.csv" "$output/kvf-ultimate-ssd-raw.csv"
  cp -- "$work/ssd-summary.csv" "$output/kvf-ultimate-ssd-summary.csv"
fi
status=0
