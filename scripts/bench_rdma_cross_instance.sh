#!/usr/bin/env bash
# Isolated two-node RXE/SIW experiment. Never attaches to a physical interface.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
provider=rxe
provider_module=
low_swappiness=0
snapshot_from=
reference=
buffer_bytes=0
queue_size=384
inline=64
chunk_pages=32
mtu=9000
phase=all
out=$root/.kvfabric-local/rdma-cross-instance/run
python_bin=$(command -v python3)
model=
build=$root/build-cuda-release
rounds=15
owner=${SUDO_USER:-$(id -un)}
usage() { echo "sudo bash $0 --python /absolute/python --model /checkpoint [--provider rxe|siw] [--phase ceiling|all|command] [--out DIR] [--rounds N] [-- CMD...]"; }
while (($#)); do
 case $1 in
  --buffer-bytes) buffer_bytes=$2; shift 2;;
  --queue-size) queue_size=$2; shift 2;;
  --chunk-pages) chunk_pages=$2; shift 2;;
  --inline) inline=$2; shift 2;;
  --reference) reference=$2; shift 2;;
  --snapshot-from) snapshot_from=$2; shift 2;;
  --low-swappiness) low_swappiness=1; shift;;
  --provider) provider=$2; shift 2;; --provider-module) provider_module=$2; shift 2;; --mtu) mtu=$2; shift 2;;
  --phase) phase=$2; shift 2;; --out) out=$2; shift 2;;
  --python) python_bin=$2; shift 2;; --model) model=$2; shift 2;;
  --build) build=$2; shift 2;; --rounds) rounds=$2; shift 2;;
  --user) owner=$2; shift 2;; --) shift; break;;
  -h|--help) usage; exit;; *) usage >&2; exit 2;;
 esac
done
[[ $EUID == 0 ]] || { echo "Requires root for netns/RDMA_NEWLINK; rerun with sudo." >&2; exit 1; }
[[ $provider == rxe || $provider == siw ]] || exit 2
[[ $mtu == 4200 || $mtu == 9000 ]] || exit 2
[[ $phase == ceiling || $phase == all || $phase == workload || $phase == faults || $phase == tune || $phase == replay || $phase == final || $phase == reference || $phase == command ]] || exit 2
[[ $rounds =~ ^[1-9][0-9]*$ ]] || exit 2
for tool in ip rdma ethtool modprobe timeout runuser flock; do command -v "$tool" >/dev/null; done
exec 9>/run/lock/kvfabric-cross-instance.lock
flock -n 9 || { echo "Another cross-instance experiment is active" >&2; exit 1; }
# Do not reuse or remove pre-existing objects with these names.
for ns in ns1 ns2; do
 if ip netns list | awk '{print $1}' | grep -qx "$ns"; then echo "$ns already exists; refusing" >&2; exit 1; fi
done
if ip link show veth1 &>/dev/null || ip link show veth2 &>/dev/null || [[ -n $(rdma link show) ]]; then
 echo "Existing veth name or RDMA device; refusing global netns mode change" >&2; exit 1
fi
[[ ! -e $out/environment.txt ]] || { echo "Use a fresh output directory" >&2; exit 1; }
mkdir -p "$out"
chown "$owner:$(id -gn "$owner")" "$out"
old_mode=$(rdma system show | awk '{for(i=1;i<=NF;i++)if($i=="netns")print $(i+1)}')
[[ $old_mode == shared || $old_mode == exclusive ]] || exit 1
module=rdma_rxe
[[ $provider == siw ]] && module=siw
lsmod | awk 'NR>1 {print $1}' > "$out/modules-before.txt"
old_swappiness=$(cat /proc/sys/vm/swappiness)
core_reloaded=0
loaded=0; created1=0; created2=0
cleanup() {
 local status=$?
 trap - EXIT INT TERM
 set +e
 for ns in ns1 ns2; do
  if { [[ $ns == ns1 && $created1 == 1 ]] || [[ $ns == ns2 && $created2 == 1 ]]; }; then
   ip netns pids "$ns" | xargs -r kill -TERM
  fi
 done
 sleep 1
 for ns in ns1 ns2; do
  if { [[ $ns == ns1 && $created1 == 1 ]] || [[ $ns == ns2 && $created2 == 1 ]]; }; then
   ip netns pids "$ns" | xargs -r kill -KILL
   ip netns exec "$ns" rdma link del "${provider}$( [[ $ns == ns1 ]] && echo 1 || echo 2 )" 2>/dev/null
   ip netns del "$ns"
  fi
 done
 # A provider that cannot move its IB device may leave it in init_net.
 # These names were absent at entry and are exclusively owned by this run.
 rdma link del "${provider}1" 2>/dev/null
 rdma link del "${provider}2" 2>/dev/null
 ip link del veth1 2>/dev/null
 if [[ -z $(rdma link show) ]]; then
  for m in ib_iser rpcrdma rdma_rxe siw rdma_ucm rdma_cm ib_cm iw_cm ib_uverbs; do
   if ! grep -qx "$m" "$out/modules-before.txt"; then modprobe -r "$m" 2>/dev/null; fi
  done
 fi
 if [[ $core_reloaded == 1 ]]; then
  # Only unload experiment-added dependencies, plus the idle core we reloaded.
  for m in ib_iser rpcrdma rdma_ucm rdma_cm ib_cm iw_cm ib_uverbs; do
   if ! grep -qx "$m" "$out/modules-before.txt"; then modprobe -r "$m" 2>/dev/null; fi
  done
  if modprobe -r ib_core; then
   modprobe ib_core netns_mode=$( [[ $old_mode == shared ]] && echo 1 || echo 0 )
  else
   echo "ERROR: idle-core restoration failed; inspect modules" >&2
   status=1
  fi
 else
  rdma system set netns "$old_mode" || status=1
 fi
 [[ $low_swappiness == 0 ]] || sysctl -q -w vm.swappiness="$old_swappiness"
 { date -Is; rdma system show; rdma link show; ip netns list; } > "$out/cleanup.txt"
 chown -R "$owner:$(id -gn "$owner")" "$out"
 exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if [[ $low_swappiness == 1 ]]; then
 printf '%s\n' "$old_swappiness" > "$out/swappiness-before.txt"
 sysctl -q -w vm.swappiness=1
fi
if ! rdma system set netns exclusive; then
 # Linux refuses a runtime mode change when unrelated namespaces exist.
 # Reload only a completely unused core, never stop any unrelated process.
 [[ -d /sys/module/ib_core && $(cat /sys/module/ib_core/refcnt) == 0 ]] || exit 1
 modprobe -r ib_core
 core_reloaded=1
 modprobe ib_core netns_mode=0
fi
[[ -d /sys/module/$module ]] || loaded=1
if [[ -n $provider_module ]]; then
 [[ $provider == siw && -f $provider_module ]] || { echo "Custom module is restricted to SIW" >&2; exit 2; }
 modprobe ib_uverbs
 modprobe iw_cm
 modprobe libcrc32c
 # Never replace a provider with active resources.
 if [[ -d /sys/module/siw ]]; then
  [[ $(cat /sys/module/siw/refcnt) == 0 ]] || exit 1
  modprobe -r siw
 fi
 sha256sum "$provider_module" > "$out/provider-module.sha256"
 insmod "$provider_module"
else
 modprobe "$module"
fi
ip netns add ns1; created1=1
ip netns add ns2; created2=1
ip link add veth1 type veth peer name veth2
ip link set veth1 netns ns1
ip link set veth2 netns ns2
for n in 1 2; do
 ip -n ns$n link set lo up
 ip -n ns$n address add 10.10.0.$n/24 dev veth$n
 ip -n ns$n link set veth$n mtu "$mtu" up
 ip netns exec ns$n ethtool -K veth$n tx off rx off > "$out/offload$n.txt" 2>&1
 done
for n in 1 2; do
 ip netns exec ns$n rdma link add "$provider$n" type "$provider" netdev veth$n
 # Linux 6.8 registers a new RXE device in init_net, even when the
 # creating netdev lives in another namespace. Move the IB device explicitly.
 if rdma dev show "$provider$n" &>/dev/null; then
  rdma dev set "$provider$n" netns ns$n
 fi
 done
{
 date -Is; uname -r; rdma system show
 for n in 1 2; do
  ip -n ns$n -details link show
  ip netns exec ns$n rdma link show
  ip netns exec ns$n ibv_devinfo
  ip netns exec ns$n ethtool -k veth$n
 done
} > "$out/environment.txt" 2>&1
ip netns exec ns2 ping -c 2 -W 2 10.10.0.1 > "$out/ping.txt"
if [[ $phase == command ]]; then
 "$@"
else
 extra_args=(--chunk-pages "$chunk_pages" --buffer-bytes "$buffer_bytes" --queue-size "$queue_size" --inline "$inline")
 [[ -z $reference ]] || extra_args+=(--reference "$reference")
 [[ -z $snapshot_from ]] || extra_args+=(--snapshot-from "$snapshot_from")
 "$python_bin" "$root/scripts/bench_rdma_cross_instance.py" orchestrate \
  --out "$out" --build "$build" --model "$model" --rounds "$rounds" \
  --python "$python_bin" --user "$owner" --provider "$provider" --phase "$phase" "${extra_args[@]}"
fi
