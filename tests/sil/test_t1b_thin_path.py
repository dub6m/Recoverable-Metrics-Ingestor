"""T1b: the thinnest end-to-end path, happy case only.

harness -> vcan -> controller -> state frame -> gateway -> Mosquitto -> collector -> store,
then a fresh process queries the store.
"""

import subprocess

import can
import cantools

import lab


def query(wal, signal):
    out = subprocess.run([str(lab.BUILD / "metrics_cli"), str(wal), "query", signal, "0"],
                         capture_output=True, text=True, check=True).stdout
    return [line.split() for line in out.splitlines()[1:] if line.strip()]


def test_command_and_battery_reading_reach_the_store(vcan, logs):
    db = cantools.database.load_file(lab.REPO / "docs" / "lab.dbc")
    wal = logs / "store.wal"
    broker = lab.Broker(logs).start()
    collector = lab.Process("collector", [str(lab.BUILD / "mqtt_collector"), "--broker-port", str(broker.port),
                                          "--wal", str(wal), "--trace", str(logs / "collector.jsonl")], logs).start()
    lab.wait_until(lambda: any(e["event"] == "subscribed" for e in lab.read_jsonl(logs / "collector.jsonl")),
                   5, "collector subscription")
    controller = lab.Process("controller", [str(lab.BUILD / "lighting_controller_can"), "--iface", vcan,
                                            "--trace", str(logs / "controller.jsonl")], logs).start()
    gateway = lab.Process("gateway", [lab.PYTHON, str(lab.REPO / "gateway" / "can_mqtt.py"), "--iface", vcan,
                                      "--broker-port", str(broker.port), "--run-id", "t1b",
                                      "--trace", str(logs / "gateway.jsonl")], logs).start()
    bus = can.Bus(interface="socketcan", channel=vcan)
    try:
        vehicle = db.get_message_by_name("VehicleState")
        command = db.get_message_by_name("LampCommand")
        alive = 0

        def lamp_on_stored():
            nonlocal alive
            alive = (alive + 1) % 16
            bus.send(can.Message(arbitration_id=vehicle.frame_id, is_extended_id=False, data=vehicle.encode(
                {"Ignition": 1, "ProtocolVersion": 1, "BatteryVoltage": 12.34})))
            bus.send(can.Message(arbitration_id=command.frame_id, is_extended_id=False, data=command.encode(
                {"LampRequest": 1, "ProtocolVersion": 1, "AliveCounter": alive})))
            return any(value == "1" for _, value in query(wal, "lamp.on"))

        lab.wait_until(lamp_on_stored, 10, "lamp.on = 1 in the store", interval=0.1)
        battery = query(wal, "vehicle.battery_v")
        assert battery and abs(float(battery[0][1]) - 12.34) < 1e-9
    finally:
        bus.shutdown()
        for process in (gateway, controller, collector):
            process.stop()
        broker.stop()
