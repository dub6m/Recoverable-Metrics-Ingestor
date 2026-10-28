"""Shared helpers for the software-in-the-loop tests: real processes, real
vcan interfaces and real brokers, with bounded waits instead of bare sleeps."""

from __future__ import annotations

import json
import os
import secrets
import signal
import socket
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

import can

REPO = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("LAB_BUILD_DIR", REPO / "build"))
PYTHON = sys.executable


def wait_until(predicate: Callable[[], object], timeout: float, what: str, interval: float = 0.02):
    """Poll predicate until it returns something truthy or the deadline passes."""
    deadline = time.monotonic() + timeout
    while True:
        result = predicate()
        if result:
            return result
        if time.monotonic() >= deadline:
            raise AssertionError(f"timed out after {timeout:.1f}s waiting for {what}")
        time.sleep(interval)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def unique_name(prefix: str) -> str:
    return prefix + secrets.token_hex(3)


# ---------------------------------------------------------------- vcan

def vcan_up(name: str) -> None:
    cmd = ["sudo", "-n", str(REPO / "scripts" / "vcan.sh"), "up", name]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"could not create vcan interface {name!r} (exit {result.returncode}): "
            f"{result.stderr.strip() or result.stdout.strip()}. "
            "The SIL tests need Linux, the vcan kernel module and passwordless sudo "
            "for scripts/vcan.sh. See README.md."
        )


def vcan_down(name: str) -> None:
    subprocess.run(["sudo", "-n", str(REPO / "scripts" / "vcan.sh"), "down", name],
                   capture_output=True)


# ---------------------------------------------------------------- processes

class Process:
    """A child process whose output goes to a log file in the test's folder."""

    def __init__(self, name: str, argv: list[str], log_dir: Path, env: Optional[dict] = None):
        self.name = name
        self.argv = argv
        self.log_path = log_dir / f"{name}.log"
        self.env = env
        self.proc: Optional[subprocess.Popen] = None

    def start(self) -> "Process":
        log = open(self.log_path, "ab")
        log.write(f"\n=== start {' '.join(self.argv)}\n".encode())
        log.flush()
        env = dict(os.environ)
        if self.env:
            env.update(self.env)
        self.proc = subprocess.Popen(self.argv, stdout=log, stderr=subprocess.STDOUT, env=env)
        log.close()
        return self

    def running(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def returncode(self) -> Optional[int]:
        return None if self.proc is None else self.proc.poll()

    def stop(self, timeout: float = 5.0) -> Optional[int]:
        if not self.running():
            return self.returncode()
        self.proc.send_signal(signal.SIGTERM)
        try:
            return self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return self.proc.wait(timeout)

    def kill(self) -> Optional[int]:
        if not self.running():
            return self.returncode()
        self.proc.kill()
        return self.proc.wait(5)

    def wait_exit(self, timeout: float) -> int:
        return self.proc.wait(timeout)

    def log(self) -> str:
        try:
            return self.log_path.read_text(errors="replace")
        except FileNotFoundError:
            return ""


class Broker:
    """A private Mosquitto broker built from config/mosquitto-lab.conf."""

    def __init__(self, log_dir: Path, port: Optional[int] = None, extra_config: str = ""):
        self.port = port or free_port()
        self.config_path = log_dir / "mosquitto.conf"
        base = (REPO / "config" / "mosquitto-lab.conf").read_text()
        lines = [f"listener {self.port} 127.0.0.1" if l.startswith("listener ") else l
                 for l in base.splitlines()]
        data_dir = log_dir / "broker-data"
        data_dir.mkdir(exist_ok=True)
        lines += [f"persistence_location {data_dir}/", "log_dest stdout", "log_type all",
                  "log_timestamp true", "log_timestamp_format %Y-%m-%dT%H:%M:%S", extra_config]
        self.config_path.write_text("\n".join(lines) + "\n")
        self.process = Process("broker", ["mosquitto", "-c", str(self.config_path)], log_dir)

    def start(self) -> "Broker":
        self.process.start()
        wait_until(self._accepting, 5, f"broker on port {self.port}")
        return self

    def _accepting(self) -> bool:
        try:
            with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                return True
        except OSError:
            return False

    def stop(self):
        self.process.stop()

    def kill(self):
        self.process.kill()


# ---------------------------------------------------------------- CAN

@dataclass
class Frame:
    t: float  # monotonic receive time
    can_id: int
    data: bytes


class CanObserver:
    """Records every frame on the bus from its own socket."""

    def __init__(self, iface: str, can_ids: Optional[list[int]] = None):
        filters = [{"can_id": i, "can_mask": 0x7FF} for i in can_ids] if can_ids else None
        self.bus = can.Bus(interface="socketcan", channel=iface, can_filters=filters)
        self.frames: list[Frame] = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        while not self._stop.is_set():
            msg = self.bus.recv(timeout=0.05)
            if msg is None or msg.is_error_frame:
                continue
            with self._lock:
                self.frames.append(Frame(time.monotonic(), msg.arbitration_id, bytes(msg.data)))

    def snapshot(self, can_id: Optional[int] = None, since: float = 0.0) -> list[Frame]:
        with self._lock:
            return [f for f in self.frames
                    if (can_id is None or f.can_id == can_id) and f.t >= since]

    def close(self):
        self._stop.set()
        self._thread.join(2)
        self.bus.shutdown()


LAMP_COMMAND_ID = 0x100
VEHICLE_STATE_ID = 0x110
LAMP_STATE_ID = 0x200
MODES = {0: "no_command", 1: "normal", 2: "expired"}


class Harness:
    """Sends lab frames, encoded from docs/lab.dbc with cantools."""

    def __init__(self, iface: str):
        import cantools
        self.db = cantools.database.load_file(REPO / "docs" / "lab.dbc")
        self.bus = can.Bus(interface="socketcan", channel=iface)

    def send_raw(self, can_id: int, data: bytes):
        self.bus.send(can.Message(arbitration_id=can_id, data=data, is_extended_id=False))

    def send_command(self, lamp: bool, alive: int):
        data = self.db.encode_message("LampCommand", {"LampRequest": int(lamp), "ProtocolVersion": 1,
                                                       "AliveCounter": alive})
        self.send_raw(LAMP_COMMAND_ID, data)

    def send_vehicle(self, ignition: bool, battery_v: float):
        data = self.db.encode_message("VehicleState", {"Ignition": int(ignition), "ProtocolVersion": 1,
                                                        "BatteryVoltage": battery_v})
        self.send_raw(VEHICLE_STATE_ID, data)

    def decode_state(self, frame: Frame) -> dict:
        fields = self.db.decode_message(LAMP_STATE_ID, frame.data, decode_choices=False)
        return {"lamp_on": bool(fields["LampOn"]), "fallback": bool(fields["FallbackActive"]),
                "mode": MODES[int(fields["Mode"])], "alive": int(fields["LastAliveCounter"]),
                "alive_valid": bool(fields["AliveValid"])}

    def close(self):
        self.bus.shutdown()


class CyclicSender:
    """Repeats a send function every period until stopped (e.g. vehicle state every 100 ms)."""

    def __init__(self, send: Callable[[], None], period: float = 0.1):
        self.send = send
        self.period = period
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        while not self._stop.is_set():
            self.send()
            self._stop.wait(self.period)

    def stop(self):
        self._stop.set()
        self._thread.join(2)


def controller_process(iface: str, logs: Path, name: str = "controller") -> Process:
    return Process(name, [str(BUILD / "lighting_controller_can"), "--iface", iface,
                          "--trace", str(logs / f"{name}.jsonl")], logs)


def count_events(path: Path, event: str) -> int:
    return sum(1 for e in read_jsonl(path) if e.get("event") == event)


def dump(wal: Path) -> list[dict]:
    """Every stored reading, read by a fresh metrics_cli process."""
    result = subprocess.run([str(BUILD / "metrics_cli"), str(wal), "dump"], capture_output=True, text=True)
    if result.returncode != 0:
        raise AssertionError(f"metrics_cli dump failed: {result.stderr}")
    return [json.loads(line) for line in result.stdout.splitlines() if line.strip()]


class Pipeline:
    """Broker, collector, controller and gateway as real processes on one vcan."""

    def __init__(self, iface: str, logs: Path, run_id: str, queue_limit: int = 1000):
        self.iface = iface
        self.logs = logs
        self.run_id = run_id
        self.queue_limit = queue_limit
        self.wal = logs / "store.wal"
        self.broker = Broker(logs)
        self.collector_trace = logs / "collector.jsonl"
        self.gateway_trace = logs / "gateway.jsonl"
        self.drop_log = logs / "drops.jsonl"
        self.collector: Optional[Process] = None
        self.controller: Optional[Process] = None
        self.gateway: Optional[Process] = None

    def start_broker(self):
        self.broker.start()

    def start_collector(self, env: Optional[dict] = None, wait: bool = True):
        before = count_events(self.collector_trace, "subscribed")
        self.collector = Process("collector", [str(BUILD / "mqtt_collector"), "--broker-port", str(self.broker.port),
                                               "--wal", str(self.wal), "--client-id", f"collector-{self.run_id}",
                                               "--trace", str(self.collector_trace)], self.logs, env).start()
        if wait:
            wait_until(lambda: count_events(self.collector_trace, "subscribed") > before, 5, "collector subscription")

    def start_controller(self):
        self.controller = controller_process(self.iface, self.logs).start()

    def start_gateway(self, env: Optional[dict] = None):
        before = count_events(self.gateway_trace, "broker_connected")
        self.gateway = Process("gateway", [PYTHON, str(REPO / "gateway" / "can_mqtt.py"), "--iface", self.iface,
                                           "--broker-port", str(self.broker.port), "--run-id", self.run_id,
                                           "--trace", str(self.gateway_trace), "--drop-log", str(self.drop_log),
                                           "--queue-limit", str(self.queue_limit)], self.logs, env).start()
        wait_until(lambda: count_events(self.gateway_trace, "broker_connected") > before, 5, "gateway connection")

    def start_all(self):
        self.start_broker()
        self.start_collector()
        self.start_controller()
        self.start_gateway()

    def stop_all(self):
        for process in (self.gateway, self.controller, self.collector):
            if process:
                process.stop()
        self.broker.stop()

    def produced(self) -> list[dict]:
        return [e for e in read_jsonl(self.gateway_trace) if e["event"] == "produced"]

    def dropped(self) -> list[dict]:
        return read_jsonl(self.drop_log)

    def unacknowledged_at_exit(self) -> set:
        return {(e["gateway_epoch"], seq) for e in read_jsonl(self.gateway_trace)
                if e["event"] == "stopped" for seq in e.get("unacknowledged", [])}

    def stored(self) -> list[dict]:
        return [r for r in dump(self.wal) if r["run_id"] == self.run_id]

    def account(self) -> "Accounting":
        return Accounting(self.produced(), self.stored(), self.dropped(), self.unacknowledged_at_exit())


@dataclass
class Accounting:
    """Every reading the gateway produced must be stored (with the same
    signal, timestamp and value) or listed in the drop log."""

    produced_rows: list
    stored_rows: list
    dropped_rows: list
    unacknowledged_at_exit: set

    @staticmethod
    def key(row):
        return (row["gateway_epoch"], row["sequence"])

    @property
    def produced(self):
        return {self.key(r): r for r in self.produced_rows}

    @property
    def stored(self):
        return {self.key(r): r for r in self.stored_rows}

    @property
    def dropped(self):
        return {self.key(r) for r in self.dropped_rows}

    @property
    def missing(self) -> list:
        stored, dropped = self.stored, self.dropped
        return sorted(k for k in self.produced if k not in stored and k not in dropped)

    @property
    def mismatched(self) -> list:
        produced = self.produced
        return [k for k, row in self.stored.items() if k in produced and any(
            row[f] != produced[k][f] for f in ("signal", "timestamp", "value"))]

    @property
    def unexpected(self) -> list:
        produced = self.produced
        return [k for k in self.stored if k not in produced]

    @property
    def stored_twice(self) -> int:
        return len(self.stored_rows) - len(self.stored)

    def summary(self) -> dict:
        return {"produced": len(self.produced), "stored": len(self.stored), "dropped": len(self.dropped),
                "missing": len(self.missing), "mismatched": len(self.mismatched),
                "unexpected": len(self.unexpected), "stored_twice": self.stored_twice}


class TelemetryClient:
    """Publishes readings like the gateway and collects application receipts."""

    def __init__(self, port: int, run_id: str):
        import paho.mqtt.client as mqtt
        self.mqtt = mqtt
        self.run_id = run_id
        self.epoch = "client" + secrets.token_hex(4)
        self.receipts: list[dict] = []
        self._lock = threading.Lock()
        subscribed = threading.Event()
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=unique_name("testclient-"))
        self.client.on_connect = lambda c, u, f, rc, p: c.subscribe(f"vtlab/v1/{run_id}/receipts", 1)
        self.client.on_subscribe = lambda c, u, mid, rcs, p: subscribed.set()
        self.client.on_message = self._on_message
        self.client.connect("127.0.0.1", port)
        self.client.loop_start()
        if not subscribed.wait(5):
            raise AssertionError("test client could not subscribe to receipts")

    def _on_message(self, client, userdata, message):
        with self._lock:
            self.receipts.append(json.loads(message.payload))

    def reading(self, sequence: int, value: float = 1.5, signal: str = "test.value", timestamp: int = 1000) -> dict:
        return {"v": 1, "run_id": self.run_id, "gateway_epoch": self.epoch, "sequence": sequence,
                "signal": signal, "timestamp": timestamp, "value": value}

    def publish(self, reading: Optional[dict] = None, topic: Optional[str] = None, payload: Optional[str] = None):
        if topic is None:
            topic = f"vtlab/v1/{self.run_id}/telemetry/{reading['signal']}"
        if payload is None:
            payload = json.dumps(reading)
        self.client.publish(topic, payload, qos=1).wait_for_publish(5)

    def receipts_for(self, sequence: int) -> list[dict]:
        with self._lock:
            return [r for r in self.receipts
                    if r.get("gateway_epoch") == self.epoch and r.get("sequence") == sequence]

    def wait_receipt(self, sequence: int, timeout: float = 3.0, count: int = 1) -> Optional[dict]:
        try:
            wait_until(lambda: len(self.receipts_for(sequence)) >= count, timeout, f"receipt for {sequence}")
        except AssertionError:
            return None
        return self.receipts_for(sequence)[count - 1]

    def close(self):
        self.client.loop_stop()
        self.client.disconnect()


class Traffic:
    """The harness side of a running vehicle: vehicle state and lamp commands
    every 100 ms, with a rolling alive counter."""

    def __init__(self, harness: Harness, battery_v: float = 12.34, lamp: bool = True):
        self.harness = harness
        self.battery_v = battery_v
        self.lamp = lamp
        self.alive = 0
        self.commands_sent = 0
        self.last_command_wall = 0.0
        self.vehicle = CyclicSender(lambda: harness.send_vehicle(True, self.battery_v))
        self.commands: Optional[CyclicSender] = None
        self.start_commands()

    def _send_command(self):
        self.alive = (self.alive + 1) % 16
        self.commands_sent += 1
        self.harness.send_command(self.lamp, self.alive)
        self.last_command_wall = time.time()

    def start_commands(self):
        if self.commands is None:
            self.commands = CyclicSender(self._send_command)

    def stop_commands(self):
        if self.commands:
            self.commands.stop()
            self.commands = None

    def stop(self):
        self.stop_commands()
        self.vehicle.stop()


def read_jsonl(path: Path) -> list[dict]:
    if not path.exists():
        return []
    rows = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if line:
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                pass  # a line being written right now
    return rows
