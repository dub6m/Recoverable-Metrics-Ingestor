"""T3: the expected-transition table, replayed in real time against the real
controller process over vcan. The same table drives the exact-time C++ tests
(tests/ControllerTests.cpp), so both must agree with what the controller sends."""

import json
import time

import pytest

import lab

TABLE = json.loads((lab.REPO / "tests" / "expected" / "lamp_transitions.json").read_text())
SIL_SCENARIOS = [s for s in TABLE["scenarios"] if s["sil"]]
SETTLE = 0.03  # state changes reach the bus within one 10 ms tick; checks look 30 ms later


def state_at(observer, harness, moment):
    """The controller's state at a moment: the latest state frame sent by then.
    Every change is sent at once, so the latest frame is the current state."""
    frames = [f for f in observer.snapshot(lab.LAMP_STATE_ID) if f.t <= moment]
    assert frames, "no state frame received yet"
    return harness.decode_state(frames[-1])


@pytest.mark.parametrize("scenario", SIL_SCENARIOS, ids=[s["name"] for s in SIL_SCENARIOS])
def test_controller_follows_expected_table(scenario, vcan, logs):
    observer = lab.CanObserver(vcan, [lab.LAMP_STATE_ID])
    harness = lab.Harness(vcan)
    controller = lab.controller_process(vcan, logs).start()
    try:
        lab.wait_until(lambda: observer.snapshot(lab.LAMP_STATE_ID), 3, "first state frame")
        start = time.monotonic()
        for step in scenario["steps"]:
            at = start + step["at_ms"] / 1000
            if "expect" in step:
                at += SETTLE
            time.sleep(max(0.0, at - time.monotonic()))
            if "vehicle" in step:
                harness.send_vehicle(step["vehicle"]["ignition"], step["vehicle"]["battery_v"])
            elif "command" in step:
                harness.send_command(step["command"]["lamp"], step["command"]["alive"])
            elif "raw" in step:
                harness.send_raw(int(step["raw"]["id"], 16), bytes.fromhex(step["raw"]["data"]))
            elif "expect" in step:
                actual = state_at(observer, harness, time.monotonic())
                expected = step["expect"]
                assert {k: actual[k] for k in expected} == expected, \
                    f"{scenario['name']} @{step['at_ms']}ms"
    finally:
        controller.stop()
        harness.close()
        observer.close()

    trace = lab.read_jsonl(logs / "controller.jsonl")
    raw_steps = [s for s in scenario["steps"] if "raw" in s and s["raw"]["id"] in ("0x100", "0x110")]
    rejected = [e for e in trace if e["event"] == "rejected"]
    assert len(rejected) == len(raw_steps), f"controller trace: {rejected}"


def test_timeout_fires_with_no_further_frames_and_is_traced(vcan, logs):
    """Commands simply stop. Nothing else arrives on the bus, so only the
    controller's own timed wake-up can produce the fallback."""
    observer = lab.CanObserver(vcan, [lab.LAMP_STATE_ID])
    harness = lab.Harness(vcan)
    controller = lab.controller_process(vcan, logs).start()
    try:
        lab.wait_until(lambda: observer.snapshot(lab.LAMP_STATE_ID), 3, "first state frame")
        harness.send_vehicle(True, 12.5)
        harness.send_command(True, 1)
        sent = time.monotonic()
        lab.wait_until(lambda: state_at(observer, harness, time.monotonic())["lamp_on"], 1, "lamp on")
        lab.wait_until(lambda: state_at(observer, harness, time.monotonic())["mode"] == "expired", 1,
                       "fallback after commands stop")
        expired_at = next(f.t for f in observer.snapshot(lab.LAMP_STATE_ID, since=sent)
                          if harness.decode_state(f)["mode"] == "expired")
        assert 0.29 <= expired_at - sent <= 0.36, f"fallback after {expired_at - sent:.3f}s"
    finally:
        controller.stop()
        harness.close()
        observer.close()
    events = [e["event"] for e in lab.read_jsonl(logs / "controller.jsonl")]
    assert "timeout" in events


def test_state_frames_repeat_every_100_ms(vcan, logs):
    observer = lab.CanObserver(vcan, [lab.LAMP_STATE_ID])
    controller = lab.controller_process(vcan, logs).start()
    try:
        lab.wait_until(lambda: len(observer.snapshot(lab.LAMP_STATE_ID)) >= 11, 3, "11 state frames")
        times = [f.t for f in observer.snapshot(lab.LAMP_STATE_ID)][:11]
        gaps = [b - a for a, b in zip(times, times[1:])]
        assert all(0.08 <= g <= 0.13 for g in gaps), gaps
    finally:
        controller.stop()
        observer.close()
