#!/usr/bin/env bash
# T0 spike: prove vcan, a lab Mosquitto broker, and record libmosquitto's PUBACK timing.
# Run inside the Linux VM from the repository root: sudo -v && scripts/t0/run_t0.sh
set -euo pipefail

out="artifacts/t0"
work="$(mktemp -d)"
trap 'kill $(jobs -p) 2>/dev/null || true; sudo scripts/vcan.sh down vcant0 || true; rm -rf "$work"' EXIT
mkdir -p "$out"

{
    echo "# T0 environment"
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "kernel: $(uname -srm)"
    echo "os: $(. /etc/os-release && echo "$PRETTY_NAME")"
    echo "g++: $(g++ --version | head -1)"
    echo "cmake: $(cmake --version | head -1)"
    echo "mosquitto: $(mosquitto -h 2>&1 | head -1)"
    echo "packages:"
    dpkg-query -W -f '  ${Package} ${Version}\n' can-utils libmosquitto-dev nlohmann-json3-dev "linux-modules-extra-$(uname -r)"
} >"$out/environment.txt"

# 1. A frame crosses vcan and an independent receiver records it.
sudo scripts/vcan.sh up vcant0
candump -L -n 1 vcant0 >"$work/candump.txt" &
dump_pid=$!
sleep 0.5
cansend vcant0 123#DEADBEEF01020304
wait "$dump_pid"
cp "$work/candump.txt" "$out/vcan_capture.txt"
grep -q 'vcant0 123#DEADBEEF01020304' "$out/vcan_capture.txt"
echo "vcan: frame 123#DEADBEEF01020304 captured" >>"$out/environment.txt"

# 2. Lab broker starts from the repository config on a private port.
port=18830
sed "s/^listener .*/listener $port 127.0.0.1/" config/mosquitto-lab.conf >"$work/broker.conf"
cat >>"$work/broker.conf" <<EOF
log_dest stdout
log_type all
log_timestamp true
log_timestamp_format %Y-%m-%dT%H:%M:%S
EOF
mosquitto -c "$work/broker.conf" >"$work/broker.log" 2>&1 &
sleep 0.5

# 3. PUBACK timing: the probe's handler holds the message for 4 seconds.
gcc -O1 -o "$work/puback_probe" scripts/t0/puback_probe.c -lmosquitto
"$work/puback_probe" "$port" >"$work/probe.txt" 2>&1 &
probe_pid=$!
sleep 1
mosquitto_pub -h 127.0.0.1 -p "$port" -q 1 -t t0/probe -m hello
wait "$probe_pid"
sleep 0.3
cp "$work/probe.txt" "$out/puback_probe.txt"
grep -E 'PUBACK|PUBLISH|t0-probe' "$work/broker.log" >"$out/puback_broker_log.txt" || true
