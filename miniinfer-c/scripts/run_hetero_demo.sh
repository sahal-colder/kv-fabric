#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

usage() {
  cat >&2 <<'HELP'
usage: run_hetero_demo.sh producer PREFIX NODE_A_IPV4 WORK_DIR [LAYER_BYTES]
       run_hetero_demo.sh consumer PREFIX NODE_A_IPV4 WORK_DIR [LAYER_BYTES] [TRIALS]
The producer defaults to cuda and the consumer defaults to cpu.
Set KVF_PRODUCER_DEVICE or KVF_CONSUMER_DEVICE to cpu, cuda, or cuda:ID.
HELP
  exit 2
}

[[ $# -ge 4 && $# -le 6 ]] || usage
mode=$1 prefix=$2 host=$3 work=$4
bytes=${5:-1048576}
trials=${6:-100}
[[ $bytes =~ ^[1-9][0-9]{0,6}$ && $trials =~ ^[1-9][0-9]{0,3}$ ]] || usage
control="$host:${KVF_CONTROL_PORT:-9000}"
source="$host:${KVF_DATA_PORT:-9001}"
worker_bin="$prefix/bin/miniinfer_worker"
bench_bin="$prefix/bin/kvf_bench"
[[ -x $worker_bin && -x $bench_bin ]] || usage
case "$mode" in
  producer) [[ $# -le 5 && -x $prefix/bin/kvfabricd ]] || usage ;;
  consumer) ;;
  *) usage ;;
esac

mkdir -p -- "$work"
run=$(mktemp -d "$work/$mode.XXXXXX")
"$bench_bin" fixture "$bytes" "$run"
printf 'Run files: %s\n' "$run"
worker_pid=
directory_pid=
cleanup() {
  for pid in "$worker_pid" "$directory_pid"; do
    if [[ -n $pid ]]; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

await_ready() {
  local pid=$1 log=$2 line i
  for ((i = 0; i < 300; ++i)); do
    if [[ -f $log ]] && IFS= read -r line < "$log" && [[ $line == "READY "* ]]; then
      return
    fi
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  cat "$log" >&2
  return 1
}

if [[ $mode == producer ]]; then
  "$prefix/bin/kvfabricd" --listen "$control" >"$run/directory.log" 2>"$run/directory.err" &
  directory_pid=$!
  await_ready "$directory_pid" "$run/directory.log"
  "$worker_bin" serve kvf-demo-producer "$control" "$source" "${KVF_PRODUCER_DEVICE:-cuda}" \
    "$run/namespace" "$run/tokens" "$run/tensors" >"$run/worker.log" 2>"$run/worker.err" &
  worker_pid=$!
  if ! await_ready "$worker_pid" "$run/worker.log"; then
    cat "$run/worker.err" >&2
    exit 1
  fi
  printf 'Producer ready. Directory %s, data %s, layer bytes %s\n' "$control" "$source" "$bytes"
  wait "$worker_pid"
  exit
fi

device=${KVF_CONSUMER_DEVICE:-cpu}
listen=${KVF_CONSUMER_LISTEN:-127.0.0.1:0}
printf 'trial,elapsed_us,payload_bytes\n' > "$run/latency.csv"
for ((trial = -1; trial <= trials; ++trial)); do
  if ! MINIINFER_WORKER_TIMING=1 "$worker_bin" pull "kvf-demo-$$" "$control" "$listen" "$device" \
    "$run/namespace" "$run/tokens" "$run/imported" >"$run/pull.log" 2>"$run/pull.err"; then
    cat "$run/pull.log" "$run/pull.err" >&2
    exit 1
  fi
  verification=$("$bench_bin" verify "$bytes" "$run/imported")
  read -r elapsed transferred <<< "$(awk '$1 == "TRANSFER" { print $2, $3 }' "$run/pull.err")"
  [[ $elapsed =~ ^[1-9][0-9]*$ && $transferred -eq $((4 * bytes)) ]]
  if (( trial > 0 )); then
    printf '%d,%d,%d\n' "$trial" "$elapsed" "$transferred" >> "$run/latency.csv"
  fi
done
printf '%s\n' "$verification"
tail -n +2 "$run/latency.csv" | sort -t, -k2,2n | awk -F, -v n="$trials" '
  { sum_us += $2; sum_bytes += $3 }
  NR == int((99 * n + 99) / 100) { p99 = $2 }
  END {
    if (NR != n || !sum_us) exit 1
    printf "trials=%d throughput_mib_s=%.3f p99_ms=%.3f\n", n, sum_bytes*1e6/1048576/sum_us, p99/1000
  }'
printf 'Raw samples: %s/latency.csv\n' "$run"
