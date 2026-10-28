# Vehicle Telemetry & Test Lab

A Linux software-in-the-loop lab that follows simulated vehicle signals from virtual CAN, through MQTT, into a C++ telemetry store. Tests run the real processes and break them on purpose: commands stop, the broker restarts, the collector crashes, the disk fills.

It grows out of **Recoverable Metrics Ingestor** (January 2026), a small C++ write-ahead-log store. The store is still here, under `src/`, rebuilt with checked writes. Its original command-line syntax still works.

## What runs

```
  test harness (Python) ── lamp commands, vehicle state ──▶ vcan ──▶ lighting controller (C++)
                                                             │              │
                                                             │      lamp state frames
                                                             ▼              ▼
                                                      gateway (Python: python-can, cantools)
                                                             │  MQTT, QoS 1
                                                             ▼
                                                     Mosquitto (local only)
                                                             │
                                                             ▼
                        receipts ◀── collector (C++, libmosquitto) ──▶ checked store (C++)
```

- **Lighting controller** (`controller/`): reads commands and vehicle state from a SocketCAN interface. It turns the lamp off if no fresh command arrives within 300 ms, and sends its state every 100 ms and on every change.
- **Gateway** (`gateway/can_mqtt.py`): decodes frames using `docs/lab.dbc` and publishes each signal with an identity that can't repeat: run, gateway start-up epoch, 64-bit sequence.
- **Collector** (`collector/`): checks each message strictly, saves it, and only then publishes a receipt.
- **Checked store** (`src/`): a versioned, checksummed log. Every write is checked and synced before a reading counts as saved. Recovery cuts off an incomplete final record and refuses corrupt or old-format logs.

`docs/protocol.md` has the frames, the controller rules, the MQTT messages, and what each hop can and can't lose.

## What it does not show

- No vehicle, CAN adapter or hardware. CAN here is the Linux kernel's virtual CAN (`vcan`), and the controller runs as a normal Linux process with no real-time guarantees.
- The lamp fallback is a chosen test behaviour, not a safety recommendation. There is no functional-safety analysis.
- Crash tests kill processes. They don't simulate power loss, so `fdatasync` is used but not proven against power cuts.
- One machine, a local broker with no TLS or login, and no cloud. Tens of readings per second, with no throughput claims.
- Vehicle-state frames are not timed out; ignition keeps its last value.
- No hosted CI. Verification runs on the documented Lima VM below.

## Setup

**On macOS** (Apple silicon), with [Lima](https://lima-vm.io):

```bash
brew install lima
scripts/lima/create_vm.sh            # Ubuntu 24.04 VM with this folder mounted, then runs setup
limactl shell --workdir "$PWD" vtlab
```

**On Ubuntu 24.04** directly: `scripts/setup_ubuntu.sh`. It installs the compilers, CMake, can-utils, Mosquitto, libmosquitto, nlohmann/json, GoogleTest and the `vcan` kernel module, plus a Python environment at `~/.venvs/vtlab` with the pinned packages in `requirements.txt`.

The SIL tests create their own `vcan` interfaces through `sudo -n scripts/vcan.sh`, and the full-disk test mounts and unmounts a small tmpfs with `sudo -n mount`/`umount`, so both need passwordless sudo (the Lima VM's default user has it). Without it they fail with a clear message; they never skip.

## Build and test

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
~/.venvs/vtlab/bin/python -m pytest tests/sil --junitxml=artifacts/sil.xml
```

Warnings are errors. `-DLAB_SANITIZE=ON` builds with AddressSanitizer and UndefinedBehaviorSanitizer. Debug builds include the test crash points (`LAB_CRASH_AT`); Release builds leave them out.

**Full check** — all three builds, every test, the deliberate-break check, the failure-mode checks and the demo, with raw output saved under `artifacts/<commit>/<run-id>/`:

```bash
scripts/check_all.sh
```

**Demo** — about 6 seconds of traffic through the whole pipeline. Commands flow, then stop (the lamp falls back), then resume:

```bash
scripts/run_demo.sh
```

## What the tests check

| Area | Where | What it proves |
|---|---|---|
| Store | `tests/CheckedStoreTests.cpp`, `tests/sil/test_t2_store_cli.py` | Exact values after restart (Python-generated fixture, fresh process); full disk, short writes and sync errors accept nothing; incomplete final record cut off, even across two crashes in a row; corruption and old logs refused; duplicates and conflicts detected across restarts; second writer locked out |
| Controller | `tests/ControllerTests.cpp`, `tests/sil/test_t3_controller.py`, `tests/expected/lamp_transitions.json` | One table of expected transitions, replayed with exact time in C++ and in real time against the controller on vcan: timeout boundary (300 ms on time, 301 ms fallback), repeated alive counters, invalid and unknown frames, ignition priority, recovery |
| Frame layouts | `tests/sil/test_dbc_layout.py` | The C++ codec matches `docs/lab.dbc` byte for byte |
| Pipeline | `tests/sil/test_t4_pipeline.py` | Readings arrive unchanged with receipts; invalid messages rejected; retries return the original result; conflicts rejected; crash after save then retry keeps one copy; alive-counter wrap and gateway restart lose nothing |
| Faults | `tests/sil/test_t5_faults.py` | Broker restart, outage beyond the queue limit, killed broker, killed collector, killed gateway, crash before save, real full disk (tmpfs), and commands stopping — each ends with a check that every reading the gateway produced is stored, in the drop log, or in a documented loss window |
| Test strength | `scripts/mutation_check.sh` | Five deliberate breaks (expiry, receipt before save, repeated alive counter, duplicate detection, incomplete-record cut-off); each makes named tests fail |

## Evidence

`artifacts/` keeps raw evidence:
- `t0/`: Linux, vcan and libmosquitto acknowledgement timing;
- `t4/`: broker persistence probes;
- `<commit>/<run-id>/`: full check runs, with an environment manifest, JUnit XML, logs and a summary.

## Store command line

The original syntax still works, plus a read-only `dump`:

```
metrics_cli <wal_path> ingest <metric> <timestamp> <value>
metrics_cli <wal_path> query <metric> <since_timestamp>
metrics_cli <wal_path> dump
```

Exit codes: 0 success, 1 usage or invalid input, 2 storage error. `ingest` prints `OK` only after the reading is written and synced. Logs written by the original store are refused with a message; they are not migrated.

## Layout

```
controller/   lighting controller, frame codec, SocketCAN program, frame_tool
gateway/      CAN -> MQTT gateway
collector/    MQTT collector and message checks
src/          checked store and CLI
docs/         protocol.md, lab.dbc
config/       Mosquitto lab config
tests/        GoogleTest suites, expected-transition table, fixtures, tests/sil (pytest)
scripts/      setup, VM, demo, full check, deliberate-break check, probes
artifacts/    evidence
```
