"""T5: integrated fault scenarios. Real controller, gateway, broker and
collector processes; faults are injected by stopping or killing them.

After every scenario the accounting check compares each reading the gateway
produced with the store and the drop log. A reading missing from both fails
the test unless it falls inside the documented loss window for that scenario
(docs/protocol.md, "Delivery and what can be lost")."""

import json
import os
import signal
import subprocess
import time

import lab


def run_id():
    return lab.unique_name("t5")


def settle(pipe, allowed_missing=0):
    """Wait until every produced reading is stored, dropped, or the count of
    missing readings is within what the scenario allows."""
    try:
        return lab.wait_until(lambda: (a := pipe.account()) and len(a.missing) <= allowed_missing and a, 8,
                              "readings to settle")
    except AssertionError:
        return pipe.account()


def produced_between(account, start, end):
    return [k for k, row in account.produced.items() if start <= row["t"] <= end]


def record(logs, name, account, **extra):
    (logs / f"{name}.accounting.json").write_text(json.dumps({**account.summary(), **extra}, indent=1))


def collapse(values):
    out = []
    for value in values:
        if not out or out[-1] != value:
            out.append(value)
    return out


# ---------------------------------------------------------------- baseline

def test_healthy_baseline_stores_every_reading(vcan, logs):
    pipe = lab.Pipeline(vcan, logs, run_id())
    pipe.start_all()
    harness = lab.Harness(vcan)
    traffic = lab.Traffic(harness)
    try:
        time.sleep(3)
        traffic.stop()
        pipe.controller.stop()
        account = settle(pipe)
        record(logs, "healthy", account)
        assert account.summary()["produced"] >= 100
        assert not account.missing and not account.dropped
        assert not account.mismatched and not account.unexpected and account.stored_twice == 0
    finally:
        traffic.stop()
        harness.close()
        pipe.stop_all()


# ---------------------------------------------------------------- broker

def broker_outage(vcan, logs, *, queue_limit, stop):
    pipe = lab.Pipeline(vcan, logs, run_id(), queue_limit=queue_limit)
    pipe.start_all()
    harness = lab.Harness(vcan)
    traffic = lab.Traffic(harness)
    try:
        time.sleep(1)
        t_down = time.time()
        stop(pipe.broker)
        lab.wait_until(lambda: lab.count_events(pipe.gateway_trace, "broker_disconnected"), 3,
                       "gateway to notice the outage")
        time.sleep(1.5)
        t_up = time.time()
        pipe.broker.start()
        lab.wait_until(lambda: last_connect(pipe) > t_up, 5, "gateway to reconnect")
        time.sleep(1.5)
        traffic.stop()
        pipe.controller.stop()
        return pipe, settle(pipe), t_down, t_up
    finally:
        traffic.stop()
        harness.close()
        pipe.stop_all()


def last_connect(pipe):
    times = [e["t"] for e in lab.read_jsonl(pipe.gateway_trace) if e["event"] == "broker_connected"]
    return times[-1] if times else 0.0


def assert_resumed(pipe, account):
    """Readings produced after the gateway reconnected are all stored. (Between
    the broker restarting and paho's next reconnect attempt, readings still
    queue or overflow into the drop log.)"""
    after = produced_between(account, last_connect(pipe) + 0.2, float("inf"))
    assert len(after) >= 20, "too few readings produced after the gateway reconnected"
    assert all(k in account.stored for k in after), "telemetry did not resume after reconnecting"


def test_broker_restart_queues_readings_then_resumes(vcan, logs):
    pipe, account, t_down, t_up = broker_outage(vcan, logs, queue_limit=1000, stop=lambda b: b.stop())
    record(logs, "broker_restart", account)
    queued = lab.count_events(pipe.gateway_trace, "queued")
    assert queued > 0, "gateway should have queued readings during the outage"
    during = produced_between(account, t_down + 0.2, t_up - 0.2)
    assert during and all(k in account.stored for k in during), "readings queued during the outage were lost"
    assert_resumed(pipe, account)
    assert not account.missing and not account.dropped
    assert not account.mismatched and account.stored_twice == 0


def test_broker_outage_beyond_the_queue_limit_goes_to_the_drop_log(vcan, logs):
    pipe, account, t_down, t_up = broker_outage(vcan, logs, queue_limit=10, stop=lambda b: b.stop())
    record(logs, "broker_overflow", account)
    drops = pipe.dropped()
    assert drops and all(d["reason"] == "queue_full" for d in drops)
    assert not account.missing, "every lost reading must be in the drop log"
    assert not any(k in account.stored for k in account.dropped), "a dropped reading was also stored"
    assert_resumed(pipe, account)


def test_killed_broker_restarts_and_telemetry_resumes(vcan, logs):
    pipe, account, t_down, t_up = broker_outage(vcan, logs, queue_limit=1000, stop=lambda b: b.kill())
    # Documented window: a killed broker can lose messages it had accepted but
    # not yet saved or delivered. Those must have been acknowledged before the kill.
    acked = {(e["gateway_epoch"], e["sequence"]): e["t"] for e in lab.read_jsonl(pipe.gateway_trace)
             if e["event"] == "acked"}
    outside_window = [k for k in account.missing if not (k in acked and acked[k] <= t_down + 0.05)]
    record(logs, "broker_kill", account, lost_in_window=len(account.missing) - len(outside_window))
    assert not outside_window, outside_window
    assert_resumed(pipe, account)
    assert not account.mismatched and account.stored_twice == 0


# ---------------------------------------------------------------- collector

def test_killed_collector_restarts_and_receives_what_was_published_meanwhile(vcan, logs):
    pipe = lab.Pipeline(vcan, logs, run_id())
    pipe.start_all()
    harness = lab.Harness(vcan)
    traffic = lab.Traffic(harness)
    try:
        time.sleep(1)
        t_kill = time.time()
        assert pipe.collector.kill() == -signal.SIGKILL
        time.sleep(1)
        t_restart = time.time()
        pipe.start_collector()
        time.sleep(1)
        traffic.stop()
        pipe.controller.stop()
        # Documented window (T0): the one message being handled at the kill.
        account = settle(pipe, allowed_missing=1)
        record(logs, "collector_kill", account)
        assert len(account.missing) <= 1, account.missing
        for key in account.missing:
            assert account.produced[key]["t"] <= t_kill, "a reading lost outside the documented window"
        downtime = produced_between(account, t_kill + 0.1, t_restart - 0.1)
        assert downtime and all(k in account.stored for k in downtime), \
            "readings published while the collector was down were not delivered after restart"
        assert not account.mismatched and account.stored_twice == 0
    finally:
        traffic.stop()
        harness.close()
        pipe.stop_all()


def test_killed_gateway_restarts_with_a_new_epoch_and_loses_only_unacknowledged_readings(vcan, logs):
    """The gateway kills itself (SIGKILL) after 40 publishes. Readings the broker
    had not yet acknowledged die with it (documented: nothing survives the
    gateway being killed). A restarted gateway gets a new epoch, so its
    readings never collide with the old ones."""
    pipe = lab.Pipeline(vcan, logs, run_id())
    pipe.start_broker()
    pipe.start_collector()
    pipe.start_controller()
    harness = lab.Harness(vcan)
    traffic = lab.Traffic(harness)
    try:
        pipe.start_gateway(env={"LAB_CRASH_AT": "gateway_after_publish", "LAB_CRASH_AFTER": "40"})
        assert pipe.gateway.wait_exit(10) == -signal.SIGKILL
        pipe.start_gateway()
        time.sleep(1.5)
        traffic.stop()
        pipe.controller.stop()
        pipe.gateway.stop()
        account = settle(pipe, allowed_missing=40)
        epochs = {e["gateway_epoch"] for e in lab.read_jsonl(pipe.gateway_trace) if e["event"] == "started"}
        assert len(epochs) == 2
        acked = {(e["gateway_epoch"], e["sequence"]) for e in lab.read_jsonl(pipe.gateway_trace)
                 if e["event"] == "acked"}
        outside_window = [k for k in account.missing if k in acked]
        record(logs, "gateway_kill", account, lost_unacknowledged=len(account.missing))
        assert not outside_window, f"readings the broker acknowledged were lost: {outside_window}"
        restarted = [k for k in account.produced if k[0] != account.produced_rows[0]["gateway_epoch"]]
        assert restarted and all(k in account.stored for k in restarted), "telemetry did not resume"
        assert not account.mismatched and account.stored_twice == 0
    finally:
        traffic.stop()
        harness.close()
        pipe.stop_all()


def test_reading_with_a_receipt_survives_a_collector_kill(logs):
    pipe = lab.Pipeline("unused", logs, run_id())
    pipe.start_broker()
    pipe.start_collector()
    client = lab.TelemetryClient(pipe.broker.port, pipe.run_id)
    try:
        client.publish(client.reading(1, value=12.34))
        assert client.wait_receipt(1)["status"] == "stored"
        pipe.collector.kill()
        pipe.start_collector()
        stored = pipe.stored()
        assert [(r["sequence"], r["value"]) for r in stored] == [(1, 12.34)]
    finally:
        client.close()
        pipe.stop_all()


def test_crash_before_saving_loses_only_that_message_as_t0_predicts(logs):
    """libmosquitto acknowledges before the handler runs (artifacts/t0), so a
    crash before the save loses that message and the broker does not resend
    it. No receipt was sent, so the client knows to retry."""
    pipe = lab.Pipeline("unused", logs, run_id())
    pipe.start_broker()
    pipe.start_collector(env={"LAB_CRASH_AT": "collector_before_persist"})
    client = lab.TelemetryClient(pipe.broker.port, pipe.run_id)
    try:
        client.publish(client.reading(1, value=1.5))
        assert pipe.collector.wait_exit(5) == -signal.SIGKILL
        assert client.wait_receipt(1, timeout=1) is None, "a receipt was sent for a reading that was never saved"

        pipe.start_collector()
        client.publish(client.reading(2, value=2.5))
        assert client.wait_receipt(2)["status"] == "stored"
        assert [r["sequence"] for r in pipe.stored()] == [2], "T0 predicts the broker does not resend message 1"

        client.publish(client.reading(1, value=1.5))  # the client's retry
        receipt = client.wait_receipt(1)
        assert receipt["status"] == "stored" and receipt["duplicate"] is False
        assert sorted(r["sequence"] for r in pipe.stored()) == [1, 2]
    finally:
        client.close()
        pipe.stop_all()


def test_full_disk_gives_failed_receipts_and_never_a_false_stored_receipt(logs):
    """The store's log sits on an 8 KB tmpfs, so the kernel returns a real
    'No space left on device' partway through. Every reading with a 'stored'
    receipt must be in the store; every other reading must be absent."""
    disk = logs / "small-disk"
    disk.mkdir()
    mount = subprocess.run(["sudo", "-n", "mount", "-t", "tmpfs", "-o",
                            f"size=8k,uid={os.getuid()},gid={os.getgid()},mode=0700", "tmpfs", str(disk)],
                           capture_output=True, text=True)
    assert mount.returncode == 0, f"could not mount a small tmpfs: {mount.stderr}"
    pipe = lab.Pipeline("unused", logs, run_id())
    pipe.wal = disk / "store.wal"
    client = None
    try:
        pipe.start_broker()
        pipe.start_collector()
        client = lab.TelemetryClient(pipe.broker.port, pipe.run_id)
        outcomes = {}
        for sequence in range(400):
            client.publish(client.reading(sequence, value=sequence + 0.25))
            receipt = client.wait_receipt(sequence)
            assert receipt is not None, f"no receipt for {sequence}"
            outcomes[sequence] = receipt["status"]
            if receipt["status"] == "failed":
                assert "No space left" in receipt["reason"], receipt
                if sum(1 for s in outcomes.values() if s == "failed") >= 5:
                    break
        receipts = {seq: [r["status"] for r in client.receipts_for(seq)] for seq in outcomes}
        stored = {r["sequence"]: r["value"] for r in pipe.stored()}
        said_stored = {seq for seq, statuses in receipts.items() if "stored" in statuses}
        assert said_stored, "the disk filled before anything was stored"
        assert any(s == "failed" for s in outcomes.values()), "the disk never filled"
        assert said_stored == set(stored), "a 'stored' receipt was sent for a reading that is not in the store"
        assert all(stored[seq] == seq + 0.25 for seq in stored)
    finally:
        if client:
            client.close()
        pipe.stop_all()
        subprocess.run(["sudo", "-n", "umount", str(disk)], capture_output=True)


# ---------------------------------------------------------------- commands stop

def test_commands_stopping_produce_fallback_in_the_store_and_on_the_bus(vcan, logs):
    pipe = lab.Pipeline(vcan, logs, run_id())
    observer = lab.CanObserver(vcan, [lab.LAMP_STATE_ID])
    pipe.start_all()
    lab.wait_until(lambda: any(r["signal"] == "lamp.on" for r in pipe.produced()), 2,
                   "the gateway to see the controller's startup state")
    harness = lab.Harness(vcan)
    traffic = lab.Traffic(harness)
    try:
        time.sleep(1)
        traffic.stop_commands()  # vehicle state keeps flowing; commands stop
        last_command = traffic.last_command_wall
        time.sleep(1)
        traffic.start_commands()  # alive counter keeps counting: fresh commands
        time.sleep(1)
        traffic.stop()
        pipe.controller.stop()
        account = settle(pipe)
        assert not account.missing
        record(logs, "commands_stop", account)

        stored = account.stored_rows
        lamp = [r for r in stored if r["signal"] == "lamp.on"]
        fallback = [r for r in stored if r["signal"] == "lamp.fallback"]
        # Expected (docs/protocol.md): startup fallback, on, fallback after the
        # timeout, on again after a fresh command.
        assert collapse([r["value"] for r in lamp]) == [0.0, 1.0, 0.0, 1.0]
        assert collapse([r["value"] for r in fallback]) == [1.0, 0.0, 1.0, 0.0]

        first_fallback = next(r for r in fallback if r["value"] == 1.0 and r["timestamp"] / 1000 > last_command)
        delay = first_fallback["timestamp"] / 1000 - last_command
        assert 0.29 <= delay <= 0.45, f"fallback {delay:.3f}s after the last command"

        on_bus = [harness.decode_state(f)["lamp_on"] for f in observer.snapshot(lab.LAMP_STATE_ID)]
        assert collapse(on_bus) == [False, True, False, True], "bus and store disagree"
    finally:
        traffic.stop()
        observer.close()
        harness.close()
        pipe.stop_all()
