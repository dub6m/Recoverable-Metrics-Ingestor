#!/usr/bin/env python3
"""Summarises a demo run from the store (read by metrics_cli in a fresh
process) and the controller trace.

    demo_summary.py --cli build/metrics_cli --wal store.wal --controller-trace controller.jsonl
"""

import argparse
import collections
import json
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--wal", required=True)
    parser.add_argument("--controller-trace", required=True)
    args = parser.parse_args()

    dump = subprocess.run([args.cli, args.wal, "dump"], capture_output=True, text=True, check=True).stdout
    readings = [json.loads(line) for line in dump.splitlines() if line.strip()]
    counts = collections.Counter(r["signal"] for r in readings)
    print(f"Store: {len(readings)} readings")
    for signal, count in sorted(counts.items()):
        print(f"  {signal:20} {count}")

    print("Lamp changes in the store (lamp.on, in arrival order):")
    previous = None
    start = readings[0]["timestamp"] if readings else 0
    for r in readings:
        if r["signal"] == "lamp.on" and r["value"] != previous:
            print(f"  +{(r['timestamp'] - start) / 1000:6.2f} s  lamp.on = {r['value']:g}")
            previous = r["value"]

    battery = {r["value"] for r in readings if r["signal"] == "vehicle.battery_v"}
    print(f"Battery values stored: {sorted(battery)}")

    events = [json.loads(line) for line in open(args.controller_trace) if line.strip()]
    kinds = collections.Counter(e["event"] for e in events)
    print(f"Controller trace: {kinds.get('command', 0)} commands accepted, "
          f"{kinds.get('timeout', 0)} timeout(s), {kinds.get('rejected', 0)} frames rejected")


if __name__ == "__main__":
    main()
