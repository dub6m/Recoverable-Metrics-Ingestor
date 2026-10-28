#!/usr/bin/env bash
# One-time setup for an Ubuntu 24.04 machine or VM: system packages, the vcan
# kernel module and the pinned Python environment. Needs sudo.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
venv="${LAB_VENV:-$HOME/.venvs/vtlab}"

export DEBIAN_FRONTEND=noninteractive
sudo -E apt-get update -qq
sudo -E apt-get install -y -qq \
    build-essential cmake ninja-build pkg-config git iproute2 \
    can-utils "linux-modules-extra-$(uname -r)" \
    mosquitto mosquitto-clients libmosquitto-dev \
    nlohmann-json3-dev libgtest-dev \
    python3-venv python3-pip

# Tests start their own brokers on private ports; the system service is not used.
sudo systemctl disable --now mosquitto >/dev/null 2>&1 || true
sudo modprobe vcan

python3 -m venv "$venv"
"$venv/bin/pip" install -q --upgrade pip
"$venv/bin/pip" install -q -r "$repo/requirements.txt"

echo "Setup complete. Python environment: $venv"
