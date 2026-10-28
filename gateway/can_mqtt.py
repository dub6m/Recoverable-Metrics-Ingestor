#!/usr/bin/env python3
"""Reads lamp-state and vehicle-state frames from SocketCAN and publishes each
signal as versioned MQTT telemetry (QoS 1).

    can_mqtt.py --iface vcan0 --broker-port 1883 --run-id demo
                [--trace gateway.jsonl] [--drop-log drops.jsonl] [--queue-limit 1000]

Every reading gets the identity (run_id, gateway_epoch, sequence). The epoch
is new on every start and the sequence never wraps, so no two readings share
an identity. Every reading produced is written to the trace; while the broker
is unreachable, readings wait in paho's queue (at most --queue-limit), and any
that don't fit go to the drop log with the reason. Nothing survives this
process being killed.

Test hook: LAB_CRASH_AT=gateway_after_publish kills the process with SIGKILL
after the LAB_CRASH_AFTER-th publish (default 1).
"""

from __future__ import annotations

import argparse
import json
import os
import secrets
import signal
import sys
import threading
import time
from pathlib import Path

import can
import cantools
import paho.mqtt.client as mqtt

DBC_PATH = Path(__file__).resolve().parents[1] / "docs" / "lab.dbc"
TOPIC = "vtlab/v1/{run_id}/telemetry/{signal}"
PROTOCOL_VERSION = 1

# CAN id -> list of (telemetry signal name, DBC signal name)
SIGNALS = {
    0x200: [("lamp.on", "LampOn"), ("lamp.fallback", "FallbackActive")],
    0x110: [("vehicle.ignition", "Ignition"), ("vehicle.battery_v", "BatteryVoltage")],
}


class JsonLines:
    """Append-only JSON-lines file, safe to write from paho's thread too."""

    def __init__(self, path: str | None):
        self.file = open(path, "a", buffering=1) if path else None
        self.lock = threading.Lock()

    def write(self, event: str, **fields):
        if self.file:
            line = json.dumps({"t": time.time(), "event": event, **fields})
            with self.lock:
                self.file.write(line + "\n")


def check_frame(db, msg) -> str | None:
    """Returns why a frame is invalid, or None if it is valid."""
    definition = db.get_message_by_frame_id(msg.arbitration_id)
    if len(msg.data) != definition.length:
        return f"length {len(msg.data)}, expected {definition.length}"
    fields = definition.decode(bytes(msg.data), decode_choices=False)
    if fields["ProtocolVersion"] != PROTOCOL_VERSION:
        return f"protocol version {fields['ProtocolVersion']}"
    for sig in definition.signals:
        value = fields[sig.name]
        if sig.minimum is not None and not sig.minimum <= value <= sig.maximum:
            return f"{sig.name} {value} outside {sig.minimum}..{sig.maximum}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iface", required=True)
    parser.add_argument("--broker-host", default="127.0.0.1")
    parser.add_argument("--broker-port", type=int, default=1883)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--trace")
    parser.add_argument("--drop-log")
    parser.add_argument("--queue-limit", type=int, default=1000)
    args = parser.parse_args()

    db = cantools.database.load_file(DBC_PATH)
    trace = JsonLines(args.trace)
    drops = JsonLines(args.drop_log)
    epoch = secrets.token_hex(8)
    sequence = 0
    crash_at = os.environ.get("LAB_CRASH_AT")
    crash_after = int(os.environ.get("LAB_CRASH_AFTER", "1"))

    stopping = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stopping.set())
    signal.signal(signal.SIGINT, lambda *_: stopping.set())

    # paho message id -> sequence, until the broker acknowledges. The lock is
    # never held while calling paho: paho may call on_publish while holding its
    # own lock, so an acknowledgement can arrive before we record the id.
    in_flight: dict[int, int] = {}
    early_acks: set[int] = set()
    in_flight_lock = threading.Lock()

    def on_connect(client, userdata, flags, reason_code, properties):
        trace.write("broker_connected", reason=str(reason_code))

    def on_disconnect(client, userdata, flags, reason_code, properties):
        with in_flight_lock:
            waiting = len(in_flight)
        trace.write("broker_disconnected", reason=str(reason_code), waiting=waiting)

    def on_publish(client, userdata, mid, reason_code, properties):
        with in_flight_lock:
            seq = in_flight.pop(mid, None)
            if seq is None:
                early_acks.add(mid)
        if seq is not None:
            trace.write("acked", gateway_epoch=epoch, sequence=seq)

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"gateway-{epoch}")
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_publish = on_publish
    client.max_queued_messages_set(args.queue_limit)
    client.reconnect_delay_set(min_delay=0.1, max_delay=1)
    client.connect_async(args.broker_host, args.broker_port, keepalive=30)
    client.loop_start()

    bus = can.Bus(interface="socketcan", channel=args.iface,
                  can_filters=[{"can_id": i, "can_mask": 0x7FF} for i in SIGNALS])
    trace.write("started", gateway_epoch=epoch, iface=args.iface, queue_limit=args.queue_limit)

    try:
        while not stopping.is_set():
            msg = bus.recv(timeout=0.05)
            if msg is None or msg.is_error_frame or msg.arbitration_id not in SIGNALS:
                continue
            problem = check_frame(db, msg)
            if problem:
                trace.write("invalid_frame", can_id=msg.arbitration_id, data=bytes(msg.data).hex(), reason=problem)
                continue
            fields = db.decode_message(msg.arbitration_id, bytes(msg.data), decode_choices=False)

            timestamp = int(msg.timestamp * 1000)
            for name, dbc_name in SIGNALS[msg.arbitration_id]:
                reading = {
                    "v": PROTOCOL_VERSION,
                    "run_id": args.run_id,
                    "gateway_epoch": epoch,
                    "sequence": sequence,
                    "signal": name,
                    "timestamp": timestamp,
                    "value": fields[dbc_name],
                }
                sequence += 1
                trace.write("produced", **reading)
                info = client.publish(TOPIC.format(run_id=args.run_id, signal=name), json.dumps(reading), qos=1)
                if info.rc == mqtt.MQTT_ERR_QUEUE_SIZE:
                    drops.write("dropped", reason="queue_full", **reading)
                    trace.write("dropped", reason="queue_full", gateway_epoch=epoch, sequence=reading["sequence"])
                    continue
                # SUCCESS, or NO_CONN: queued by paho and sent after reconnecting.
                with in_flight_lock:
                    acked_already = info.mid in early_acks
                    if acked_already:
                        early_acks.discard(info.mid)
                    else:
                        in_flight[info.mid] = reading["sequence"]
                if acked_already:
                    trace.write("acked", gateway_epoch=epoch, sequence=reading["sequence"])
                if info.rc == mqtt.MQTT_ERR_NO_CONN:
                    trace.write("queued", gateway_epoch=epoch, sequence=reading["sequence"])
                if crash_at == "gateway_after_publish":
                    crash_after -= 1
                    if crash_after <= 0:
                        trace.write("crash_point", point=crash_at)
                        os.kill(os.getpid(), signal.SIGKILL)
    finally:
        # On a normal stop, give the broker a moment to acknowledge what is in
        # flight. Anything still unacknowledged is listed: it ends with this process.
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            with in_flight_lock:
                if not in_flight:
                    break
            time.sleep(0.02)
        with in_flight_lock:
            unacknowledged = sorted(in_flight.values())
        trace.write("stopped", gateway_epoch=epoch, produced=sequence, unacknowledged=unacknowledged)
        bus.shutdown()
        client.loop_stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
