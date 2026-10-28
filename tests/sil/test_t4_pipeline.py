"""T4: gateway, broker and collector with identity, receipts and duplicate handling."""

import json
import signal
import time

import lab


def run_id():
    return lab.unique_name("t4")


def test_lamp_state_and_battery_reach_the_store_unchanged_with_receipts(vcan, logs):
    pipe = lab.Pipeline(vcan, logs, run_id())
    pipe.start_all()
    receipts = lab.TelemetryClient(pipe.broker.port, pipe.run_id)
    harness = lab.Harness(vcan)
    traffic = lab.Traffic(harness, battery_v=12.34)
    try:
        time.sleep(1.0)
        traffic.stop()
        pipe.controller.stop()  # no more frames: the produced set is final
        account = lab.wait_until(lambda: (a := pipe.account()) and not a.missing and a, 5,
                                 "every produced reading to be stored")
        assert account.summary()["produced"] >= 30
        assert not account.mismatched and not account.unexpected and account.stored_twice == 0

        stored = account.stored_rows
        assert any(r["signal"] == "lamp.on" and r["value"] == 1.0 for r in stored)
        assert any(r["signal"] == "lamp.fallback" and r["value"] == 0.0 for r in stored)
        battery = [r for r in stored if r["signal"] == "vehicle.battery_v"]
        assert battery and all(r["value"] == 12.34 for r in battery)

        # A "stored" receipt for every produced reading, sent after the save.
        produced = set(account.produced)
        def all_receipts():
            got = {(r["gateway_epoch"], r["sequence"]) for r in list(receipts.receipts)
                   if r.get("status") == "stored"}
            return produced <= got
        lab.wait_until(all_receipts, 5, "a stored receipt for every reading")
    finally:
        traffic.stop()
        harness.close()
        receipts.close()
        pipe.stop_all()


def test_invalid_messages_are_rejected_and_never_stored(logs):
    pipe = lab.Pipeline("unused", logs, run_id())
    pipe.start_broker()
    pipe.start_collector()
    client = lab.TelemetryClient(pipe.broker.port, pipe.run_id)
    try:
        base = f"vtlab/v1/{pipe.run_id}/telemetry"
        good = client.reading(0)
        bad = [
            (f"{base}", json.dumps(good)),                                   # no signal level
            (f"{base}/test.value", "not json"),
            (f"{base}/test.value", json.dumps({**good, "sequence": 1, "value": "1.5"})),
            (f"{base}/test.value", json.dumps({**good, "sequence": 2, "v": 2})),
            (f"{base}/other.signal", json.dumps({**good, "sequence": 3})),   # signal != topic
            (f"{base}/test.value", json.dumps({**good, "sequence": 4, "extra": 1})),
            (f"{base}/test.value", json.dumps({k: v for k, v in {**good, "sequence": 5}.items() if k != "timestamp"})),
            (f"{base}/test.value", json.dumps({**good, "sequence": -6})),
            (f"{base}/test.value", json.dumps({**good, "sequence": 7}).replace("1.5", "1e999")),
        ]
        for topic, payload in bad:
            client.publish(topic=topic, payload=payload)
        marker = client.reading(100, value=2.5)
        client.publish(marker)
        assert client.wait_receipt(100)["status"] == "stored"

        assert [r["sequence"] for r in pipe.stored()] == [100]
        rejected = [e for e in lab.read_jsonl(pipe.collector_trace) if e["event"] == "rejected"]
        assert len(rejected) == len(bad), rejected
        # Rejections with a readable identity get a "rejected" receipt.
        for sequence in (1, 2, 3, 4, 5):
            assert client.wait_receipt(sequence)["status"] == "rejected", sequence
    finally:
        client.close()
        pipe.stop_all()


def test_retry_of_an_accepted_reading_returns_the_original_result(logs):
    pipe = lab.Pipeline("unused", logs, run_id())
    pipe.start_broker()
    pipe.start_collector()
    client = lab.TelemetryClient(pipe.broker.port, pipe.run_id)
    try:
        reading = client.reading(1, value=13.8)
        client.publish(reading)
        first = client.wait_receipt(1)
        assert first["status"] == "stored" and first["duplicate"] is False
        client.publish(reading)
        second = client.wait_receipt(1, count=2)
        assert second["status"] == "stored" and second["duplicate"] is True
        stored = pipe.stored()
        assert len(stored) == 1 and stored[0]["value"] == 13.8
    finally:
        client.close()
        pipe.stop_all()


def test_conflicting_reading_with_the_same_identity_is_rejected(logs):
    pipe = lab.Pipeline("unused", logs, run_id())
    pipe.start_broker()
    pipe.start_collector()
    client = lab.TelemetryClient(pipe.broker.port, pipe.run_id)
    try:
        client.publish(client.reading(1, value=1.5))
        assert client.wait_receipt(1)["status"] == "stored"
        client.publish(client.reading(1, value=2.5))
        conflict = client.wait_receipt(1, count=2)
        assert conflict["status"] == "rejected" and "conflict" in conflict["reason"]
        stored = pipe.stored()
        assert len(stored) == 1 and stored[0]["value"] == 1.5
    finally:
        client.close()
        pipe.stop_all()


def test_crash_after_saving_before_the_receipt_then_retry_keeps_one_copy(logs):
    pipe = lab.Pipeline("unused", logs, run_id())
    pipe.start_broker()
    pipe.start_collector(env={"LAB_CRASH_AT": "collector_after_persist"})
    client = lab.TelemetryClient(pipe.broker.port, pipe.run_id)
    try:
        reading = client.reading(1, value=9.99)
        client.publish(reading)
        assert pipe.collector.wait_exit(5) == -signal.SIGKILL
        assert client.wait_receipt(1, timeout=1) is None, "no receipt: the collector died before sending it"
        assert [r["sequence"] for r in pipe.stored()] == [1], "the reading was saved before the crash"

        pipe.start_collector()
        client.publish(reading)  # the client never got a receipt, so it retries
        receipt = client.wait_receipt(1)
        assert receipt["status"] == "stored" and receipt["duplicate"] is True
        stored = pipe.stored()
        assert len(stored) == 1 and stored[0]["value"] == 9.99
    finally:
        client.close()
        pipe.stop_all()


def test_alive_counter_wrap_and_gateway_restart_store_every_distinct_reading(vcan, logs):
    pipe = lab.Pipeline(vcan, logs, run_id())
    pipe.start_all()
    harness = lab.Harness(vcan)
    traffic = lab.Traffic(harness)
    try:
        time.sleep(1.2)
        pipe.gateway.stop()       # mid-run restart: a new epoch, sequence back to 0
        pipe.start_gateway()
        time.sleep(1.2)
        assert traffic.commands_sent > 16, "the 4-bit alive counter must wrap"
        traffic.stop()
        pipe.controller.stop()
        pipe.gateway.stop()
        account = lab.wait_until(lambda: (a := pipe.account()) and not a.missing and a, 5,
                                 "every produced reading to be stored")
        epochs = {epoch for epoch, _ in account.produced}
        assert len(epochs) == 2
        for epoch in epochs:
            assert (epoch, 0) in account.stored, "sequence 0 of each epoch is a distinct reading"
        assert not account.mismatched and not account.unexpected and account.stored_twice == 0
        conflicts = [e for e in lab.read_jsonl(pipe.collector_trace) if e["event"] == "conflict"]
        assert not conflicts, conflicts
    finally:
        traffic.stop()
        harness.close()
        pipe.stop_all()
