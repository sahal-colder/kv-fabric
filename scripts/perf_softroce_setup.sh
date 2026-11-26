#!/usr/bin/env bash
# Create only task-owned host interfaces. No physical interface or default route changes.
set -euo pipefail
state=/run/kvfabric-perf-rxe.state
dev=kvf_audit_rxe
left=kvfaudit0
right=kvfaudit1
if [[ $EUID != 0 ]]; then echo "Host root is required for RDMA_NEWLINK and loading RXE." >&2; exit 1; fi
case ${1:-} in
  setup)
    [[ ! -e $state ]] || { echo "Task state already exists; inspect before retry." >&2; exit 1; }
    if ip link show "$left" &>/dev/null || ip link show "$right" &>/dev/null ||
       rdma link show | grep -q "$dev"; then echo "Task-owned names already exist; refusing reuse." >&2; exit 1; fi
    if ip -4 address show | grep -q '198\.19\.77\.'; then echo "Task subnet is already in use." >&2; exit 1; fi
    loaded=0
    [[ -d /sys/module/rdma_rxe ]] || loaded=1
    modprobe rdma_rxe
    touch "$state"
    printf '%s\n' "$loaded" > "$state"
    trap 'ip link del "$left" 2>/dev/null || true; rm -f "$state"' ERR
    ip link add "$left" type veth peer name "$right"
    ip address add 198.19.77.1/30 dev "$left"
    ip address add 198.19.77.2/30 dev "$right"
    ip link set "$left" up
    ip link set "$right" up
    rdma link add "$dev" type rxe netdev "$left"
    rdma link show "$dev/1"
    echo "READY: 198.19.77.1; after testing run this script with cleanup."
    ;;
  mtu)
    [[ -e $state ]] || { echo "No task-owned RXE state." >&2; exit 1; }
    [[ ${2:-} == 1500 || ${2:-} == 9000 ]] || { echo "Only MTU 1500 or 9000 is supported." >&2; exit 2; }
    ip link set "$left" mtu "$2"
    ip link set "$right" mtu "$2"
    ip -details link show "$left"
    rdma link show "$dev/1"
    ;;
  cleanup)
    [[ -e $state ]] || { echo "No task-owned state; nothing to remove."; exit; }
    loaded=$(cat "$state")
    rdma link del "$dev" 2>/dev/null || true
    ip link del "$left" 2>/dev/null || true
    rm -f "$state"
    if [[ $loaded == 1 ]] && ! rdma link show | grep -q 'netdev'; then
      modprobe -r rdma_rxe || true
    fi
    echo "Task RXE device and veth removed."
    ;;
  *) echo "usage: sudo bash $0 {setup|cleanup|mtu 1500|mtu 9000}" >&2; exit 2 ;;
esac
