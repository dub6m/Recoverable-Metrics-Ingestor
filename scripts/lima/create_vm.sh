#!/usr/bin/env bash
# Run on macOS: create an Ubuntu 24.04 Lima VM with this repository mounted
# read-write at the same path, then run the Linux setup inside it.
# Usage: scripts/lima/create_vm.sh [vm-name]
set -euo pipefail

name="${1:-vtlab}"
repo="$(cd "$(dirname "$0")/../.." && pwd)"

command -v limactl >/dev/null || { echo "Install Lima first: brew install lima" >&2; exit 1; }

limactl create --name="$name" --tty=false --cpus=4 --memory=4 --disk=20 \
    --set ".mounts = [{\"location\": \"$repo\", \"writable\": true}] | .containerd.user = false | .containerd.system = false" \
    template:ubuntu-24.04
limactl start "$name" --tty=false
limactl shell --workdir "$repo" "$name" -- scripts/setup_ubuntu.sh

echo "Ready. Open a shell with: limactl shell --workdir \"$repo\" $name"
