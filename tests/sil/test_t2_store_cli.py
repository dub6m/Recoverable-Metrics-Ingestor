"""T2 through the command-line tool: each ingest is its own process and each
check reads the log back in a fresh process."""

import json
import subprocess

import lab

CLI = str(lab.BUILD / "metrics_cli")


def cli(*args):
    return subprocess.run([CLI, *map(str, args)], capture_output=True, text=True)


def fixture_values():
    for line in (lab.REPO / "tests" / "fixtures" / "round_trip_values.txt").read_text().splitlines():
        if line and not line.startswith("#"):
            decimal, hex_value = line.split()
            yield decimal, hex_value


def test_values_come_back_exactly_in_a_fresh_process(logs):
    wal = logs / "store.wal"
    expected = []
    for timestamp, (decimal, hex_value) in enumerate(fixture_values()):
        result = cli(wal, "ingest", "fixture.value", timestamp, decimal)
        assert result.returncode == 0 and result.stdout == "OK\n", result.stderr
        expected.append(hex_value)

    dumped = cli(wal, "dump")
    assert dumped.returncode == 0, dumped.stderr
    values = [json.loads(line)["value"] for line in dumped.stdout.splitlines()]
    # Python parses the printed decimals itself, independently of the C++ parser.
    assert [float(v).hex() for v in values] == expected


def test_failed_ingest_exits_nonzero_and_prints_no_ok(logs):
    result = cli(logs / "missing-dir" / "store.wal", "ingest", "lamp.on", 1, "1")
    assert result.returncode == 2
    assert "OK" not in result.stdout
    assert "cannot open log" in result.stderr


def test_non_finite_value_is_rejected(logs):
    result = cli(logs / "store.wal", "ingest", "lamp.on", 1, "nan")
    assert result.returncode == 1
    assert "OK" not in result.stdout


def test_original_format_log_is_refused_and_left_unchanged(logs):
    wal = logs / "old.wal"
    wal.write_text("cpu 100 0.5\n")
    result = cli(wal, "ingest", "cpu", 200, "0.6")
    assert result.returncode == 2
    assert "original store" in result.stderr
    assert "OK" not in result.stdout
    assert wal.read_text() == "cpu 100 0.5\n"


def test_corrupt_log_refuses_query(logs):
    wal = logs / "store.wal"
    for timestamp in range(3):
        assert cli(wal, "ingest", "lamp.on", timestamp, "1").returncode == 0
    text = wal.read_text()
    wal.write_text(text.replace("lamp.on", "lamp.On", 1))
    result = cli(wal, "query", "lamp.on", 0)
    assert result.returncode == 2
    assert "corrupt record at line 2" in result.stderr
