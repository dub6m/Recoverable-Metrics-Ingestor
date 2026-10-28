#!/usr/bin/env python3
"""Demo traffic: vehicle state and lamp commands every 100 ms, then the
commands stop (the controller falls back) and resume (normal again).

    demo_traffic.py --iface vtdemo0
"""

import argparse
import threading
import time
from pathlib import Path

import can
import cantools

DBC = Path(__file__).resolve().parents[1] / "docs" / "lab.dbc"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iface", required=True)
    args = parser.parse_args()
    db = cantools.database.load_file(DBC)
    bus = can.Bus(interface="socketcan", channel=args.iface)
    state = {"alive": 0, "commands": False, "stop": False}

    def send(name, values):
        message = db.get_message_by_name(name)
        bus.send(can.Message(arbitration_id=message.frame_id, data=message.encode(values), is_extended_id=False))

    def loop():
        while not state["stop"]:
            send("VehicleState", {"Ignition": 1, "ProtocolVersion": 1, "BatteryVoltage": 12.34})
            if state["commands"]:
                state["alive"] = (state["alive"] + 1) % 16
                send("LampCommand", {"LampRequest": 1, "ProtocolVersion": 1, "AliveCounter": state["alive"]})
            time.sleep(0.1)

    sender = threading.Thread(target=loop)
    sender.start()
    phases = [("commands on: lamp should turn on", True, 2.0),
              ("commands stop: lamp should fall back to off after 300 ms", False, 1.5),
              ("commands resume: lamp should turn on again", True, 1.5)]
    try:
        for text, commands, seconds in phases:
            print(f"[demo] {text}", flush=True)
            state["commands"] = commands
            time.sleep(seconds)
    finally:
        state["stop"] = True
        sender.join()
        bus.shutdown()


if __name__ == "__main__":
    main()
