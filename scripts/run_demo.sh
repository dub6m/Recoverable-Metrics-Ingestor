#!/usr/bin/env bash
# End-to-end demo on a private vcan interface:
#   demo traffic -> vcan -> controller -> gateway -> Mosquitto -> collector -> store
# Run inside the Linux VM after building: scripts/run_demo.sh
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
build="${LAB_BUILD_DIR:-$repo/build}"
python="${LAB_PYTHON:-$HOME/.venvs/vtlab/bin/python}"
iface="vtdemo$$"
iface="${iface:0:15}"
port=18990
out="$(mktemp -d /tmp/vtlab-demo.XXXXXX)"
pids=()

cleanup() {
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    sudo -n "$repo/scripts/vcan.sh" down "$iface" || true
}
trap cleanup EXIT

for tool in "$build/lighting_controller_can" "$build/mqtt_collector" "$build/metrics_cli"; do
    [ -x "$tool" ] || { echo "missing $tool: build first (see README.md)" >&2; exit 1; }
done

echo "[demo] output folder: $out"
sudo -n "$repo/scripts/vcan.sh" up "$iface"

mkdir -p "$out/broker"
sed "s/^listener .*/listener $port 127.0.0.1/" "$repo/config/mosquitto-lab.conf" >"$out/mosquitto.conf"
echo "persistence_location $out/broker/" >>"$out/mosquitto.conf"
mosquitto -c "$out/mosquitto.conf" >"$out/broker.log" 2>&1 &
pids+=($!)
sleep 0.3

"$build/mqtt_collector" --broker-port "$port" --wal "$out/store.wal" --client-id demo-collector \
    --trace "$out/collector.jsonl" &
pids+=($!)
for _ in $(seq 50); do grep -q subscribed "$out/collector.jsonl" 2>/dev/null && break; sleep 0.1; done

"$build/lighting_controller_can" --iface "$iface" --trace "$out/controller.jsonl" &
pids+=($!)
"$python" "$repo/gateway/can_mqtt.py" --iface "$iface" --broker-port "$port" --run-id demo \
    --trace "$out/gateway.jsonl" --drop-log "$out/drops.jsonl" &
gateway=$!
pids+=($gateway)
candump -L "$iface" >"$out/candump.log" &
pids+=($!)
sleep 0.5

"$python" "$repo/scripts/demo_traffic.py" --iface "$iface"
echo "[demo] traffic ended: the lamp falls back once more before the demo stops"
sleep 0.5
kill "$gateway"
wait "$gateway" 2>/dev/null || true

echo
echo "[demo] CAN frames captured: $(wc -l <"$out/candump.log") (last 3):"
tail -n 3 "$out/candump.log" | sed 's/^/  /'
echo
"$python" "$repo/scripts/demo_summary.py" --cli "$build/metrics_cli" --wal "$out/store.wal" \
    --controller-trace "$out/controller.jsonl"
echo
echo "[demo] Traces, the CAN capture and the store are in $out"
