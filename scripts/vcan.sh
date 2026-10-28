#!/usr/bin/env bash
# Create or remove a virtual CAN interface. Needs root (or CAP_NET_ADMIN).
# Usage: scripts/vcan.sh up|down [name]
set -euo pipefail

action="${1:-}"
name="${2:-vcan0}"

case "$action" in
up)
    if ! lsmod | grep -q '^vcan'; then
        modprobe vcan || {
            echo "error: kernel module 'vcan' is not available." >&2
            echo "On Ubuntu: sudo apt-get install linux-modules-extra-\$(uname -r)" >&2
            exit 2
        }
    fi
    if ! ip link show "$name" >/dev/null 2>&1; then
        ip link add dev "$name" type vcan
    fi
    ip link set up "$name"
    ;;
down)
    if ip link show "$name" >/dev/null 2>&1; then
        ip link delete "$name"
    fi
    ;;
*)
    echo "usage: $0 up|down [name]" >&2
    exit 1
    ;;
esac
