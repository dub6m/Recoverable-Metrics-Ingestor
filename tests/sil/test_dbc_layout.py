"""The C++ frame codec matches docs/lab.dbc: same bytes when encoding, same
values when decoding, for every combination of the small fields."""

import itertools
import json
import subprocess

import cantools

import lab

DB = cantools.database.load_file(lab.REPO / "docs" / "lab.dbc")
TOOL = str(lab.BUILD / "frame_tool")


def tool(*args):
    return subprocess.run([TOOL, *map(str, args)], capture_output=True, text=True, check=True).stdout.strip()


def candump_text(can_id, data):
    return f"{can_id:03X}#{data.hex().upper()}"


def test_lamp_command_layout_matches_dbc():
    for lamp, alive in itertools.product((0, 1), range(16)):
        data = DB.encode_message("LampCommand", {"LampRequest": lamp, "ProtocolVersion": 1, "AliveCounter": alive})
        assert tool("encode-command", lamp, alive) == candump_text(0x100, data)
        assert json.loads(tool("decode", candump_text(0x100, data))) == {"LampRequest": lamp, "AliveCounter": alive}


def test_vehicle_state_layout_matches_dbc():
    for ignition, raw in itertools.product((0, 1), (600, 1234, 1250, 1799, 1800)):
        data = DB.encode_message("VehicleState", {"Ignition": ignition, "ProtocolVersion": 1,
                                                  "BatteryVoltage": raw / 100})
        assert tool("encode-vehicle", ignition, raw) == candump_text(0x110, data)
        assert json.loads(tool("decode", candump_text(0x110, data))) == {"Ignition": ignition, "BatteryRaw": raw}


def test_lamp_state_layout_matches_dbc():
    for lamp, fallback, mode, alive, valid in itertools.product((0, 1), (0, 1), (0, 1, 2), (0, 7, 15), (0, 1)):
        values = {"LampOn": lamp, "FallbackActive": fallback, "Mode": mode, "ProtocolVersion": 1,
                  "LastAliveCounter": alive, "AliveValid": valid}
        data = DB.encode_message("LampState", values)
        assert tool("encode-state", lamp, fallback, mode, alive, valid) == candump_text(0x200, data)
        decoded = json.loads(tool("decode", candump_text(0x200, data)))
        del values["ProtocolVersion"]
        assert decoded == values


def test_frame_lengths_match_dbc():
    assert DB.get_message_by_name("LampCommand").length == 2
    assert DB.get_message_by_name("VehicleState").length == 3
    assert DB.get_message_by_name("LampState").length == 2
