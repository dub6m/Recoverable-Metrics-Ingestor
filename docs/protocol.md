# Lab protocol

This lab is a software-in-the-loop setup on Linux virtual CAN (`vcan`). It is not vehicle software, and none of the behaviour below is an automotive safety recommendation. It is the behaviour the lab's tests check.

`docs/lab.dbc` is the machine-readable source for every frame below. The C++ codec (`controller/frame_codec.cpp`) hard-codes the same layouts, and `tests/sil/test_dbc_layout.py` checks the two against each other.

## CAN frames

All frames are classic CAN, 11-bit IDs, little-endian (Intel) signals. Protocol version is 1.

| ID | Name | Sender | Length | Period |
|---|---|---|---|---|
| `0x100` | LampCommand | test harness | 2 bytes | every 100 ms while commands flow |
| `0x110` | VehicleState | test harness | 3 bytes | every 100 ms |
| `0x200` | LampState | controller | 2 bytes | every 100 ms, and at once on any change |

**LampCommand (`0x100`)**

| Bits | Signal | Values |
|---|---|---|
| byte 0 bit 0 | LampRequest | 0 off, 1 on |
| byte 0 bits 1–3 | reserved | must be 0 |
| byte 0 bits 4–7 | ProtocolVersion | must be 1 |
| byte 1 bits 0–3 | AliveCounter | 0–15, rolling |
| byte 1 bits 4–7 | reserved | must be 0 |

**VehicleState (`0x110`)**

| Bits | Signal | Values |
|---|---|---|
| byte 0 bit 0 | Ignition | 0 off, 1 on |
| byte 0 bits 1–3 | reserved | must be 0 |
| byte 0 bits 4–7 | ProtocolVersion | must be 1 |
| bytes 1–2 | BatteryVoltage | raw × 0.01 V; valid 600–1800 (6.00–18.00 V) |

**LampState (`0x200`)**

| Bits | Signal | Values |
|---|---|---|
| byte 0 bit 0 | LampOn | 0 / 1 |
| byte 0 bit 1 | FallbackActive | 1 whenever the mode is not Normal |
| byte 0 bits 2–3 | Mode | 0 NoCommand, 1 Normal, 2 Expired |
| byte 0 bits 4–7 | ProtocolVersion | 1 |
| byte 1 bits 0–3 | LastAliveCounter | alive counter of the last accepted command |
| byte 1 bit 4 | AliveValid | 1 once any command has been accepted |

The state frame repeats the last accepted alive counter, so a trace can match each command to the state it caused.

## Invalid frames

The controller asks the kernel for IDs `0x100` and `0x110` only (`CAN_RAW_FILTER`). Its own state frames use `0x200`, so it never reads its own output, and other IDs never reach it.

A command or vehicle-state frame is rejected if it has the wrong length, the wrong protocol version, any reserved bit set, or a battery value out of range. A rejected frame never changes state and never resets the command timer. Each rejection is written to the controller's trace with the reason.

## Controller rules

The timeout window is **300 ms**, three command periods.

1. **Startup.** The mode is NoCommand: lamp off, fallback active. Ignition counts as off until the first valid vehicle-state frame.
2. **Fresh commands.** A command is accepted only if its alive counter differs from the last accepted command's (the first command is always accepted). Accepting a command sets the mode to Normal, records the lamp request and restarts the timer.
3. **Repeated counter.** A command with the same alive counter as the last accepted one is ignored, even if it asks for something different. It means a duplicate frame or a stuck sender, and it must not keep the lamp alive.
4. **Timeout boundary.** A command is on time while the time since the last accepted command is at most 300 ms. Fallback starts at 301 ms. A command arriving at exactly 300 ms is on time.
5. **Fallback.** When the timer runs out, the mode becomes Expired: lamp off, fallback active. The old "on" request is forgotten, so a stale command can never turn the lamp back on.
6. **Recovery.** A fresh command (new alive counter) after fallback restores Normal at once. A stuck sender repeating the old counter keeps the lamp in fallback.
7. **Ignition priority.** The lamp is on only if the mode is Normal, the last command asked for on, and ignition is on. Ignition off forces the lamp off. Commands still keep the timer alive while ignition is off, so the lamp comes back as soon as ignition returns.

The controller wakes on every frame and on a 10 ms tick, so the timeout fires even when nothing else arrives. Vehicle-state frames are not timed out in this lab; ignition keeps its last value.

These rules are written as scenarios in `tests/expected/lamp_transitions.json`. The C++ tests replay all of them with exact times; the SIL tests replay the ones marked `sil` against the real controller on `vcan`, checking at least 40 ms away from any change.

## MQTT telemetry

The gateway (`gateway/can_mqtt.py`) decodes LampState and VehicleState frames with `cantools` from `docs/lab.dbc`. It checks each frame's length, protocol version and signal ranges, and publishes one message per signal:

| Topic | Signal | Value |
|---|---|---|
| `vtlab/v1/<run_id>/telemetry/lamp.on` | LampOn | 0 / 1 |
| `vtlab/v1/<run_id>/telemetry/lamp.fallback` | FallbackActive | 0 / 1 |
| `vtlab/v1/<run_id>/telemetry/vehicle.ignition` | Ignition | 0 / 1 |
| `vtlab/v1/<run_id>/telemetry/vehicle.battery_v` | BatteryVoltage | volts, e.g. 12.34 |

Each payload is a JSON object with exactly these fields:

```json
{"v": 1, "run_id": "demo", "gateway_epoch": "3f9c0a1b2c3d4e5f", "sequence": 41,
 "signal": "vehicle.battery_v", "timestamp": 1793136069828, "value": 12.34}
```

- **Identity** is `(run_id, gateway_epoch, sequence)`. `gateway_epoch` is a new random value every time the gateway starts. `sequence` counts from 0 in each epoch and never wraps. The CAN alive counter is only for freshness and is never an identity.
- **Timestamp** is the CAN receive time in milliseconds (wall clock).
- Messages are published at QoS 1 and never retained.

### Delivery and what can be lost

| Hop | Guarantee | Loss window |
|---|---|---|
| Gateway → broker | QoS 1; paho resends unacknowledged messages after reconnecting, so duplicates are possible | While the broker is unreachable, readings wait in paho's queue (limit `--queue-limit`, default 1000). Overflow is written to the drop log with reason `queue_full`. Anything unacknowledged when the gateway stops is listed in its trace; nothing survives the gateway being killed. |
| Broker → collector | The collector uses a fixed client ID, a persistent session (clean session off) and a QoS 1 subscription. The broker saves sessions and queued messages to disk on every change, so a broker restart keeps the collector's subscription. | `max_queued_messages 1000` per offline client. A killed broker can lose messages it had accepted but not yet saved or delivered. |
| Collector → store | The reading is saved (written and synced) before its receipt is sent | libmosquitto acknowledges a message to the broker **before** the handler runs (T0). A collector crash while handling a message loses that one message; the broker won't resend it. Messages not yet read are resent after reconnecting. |

The gateway does not wait for receipts. Its readings are accounted for instead: every reading it produced is in its trace, and the SIL tests check that each one is stored, in the drop log, or inside one of the windows above.

### Receipts

After each message, the collector publishes a receipt to `vtlab/v1/<run_id>/receipts`:

```json
{"v": 1, "run_id": "demo", "gateway_epoch": "...", "sequence": 41, "status": "stored", "duplicate": false}
```

| status | Meaning |
|---|---|
| `stored`, `duplicate: false` | Written and synced just now |
| `stored`, `duplicate: true` | Same identity and same reading already stored; nothing new written (the original outcome) |
| `rejected` | Invalid message, or same identity with a different reading (`reason` says which) |
| `failed` | Storage error; nothing accepted. Retry later with the same identity |

A broker acknowledgement (PUBACK) is not a receipt. A client that needs to know its reading was saved waits for the receipt and retries with the same identity if none arrives. Messages with an unreadable identity (bad topic, not JSON) get no receipt and are logged in the collector's trace.

## Store

The collector writes to the checked store (`src/`). It writes each record, syncs it, and only then updates memory and sends the receipt. Recovery cuts off an incomplete final record before appending, refuses corrupt records anywhere else, and refuses old-format logs. The crash tests kill processes (SIGKILL); they do not simulate power loss.
