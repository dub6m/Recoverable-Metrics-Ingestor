# T0 — Linux and library spike (2026-10-27)

Run with `scripts/t0/run_t0.sh` inside an Ubuntu 24.04 VM (Lima, Apple Virtualization, arm64). Raw output is in this folder.

| Check | Result | Evidence |
|---|---|---|
| `vcan` loads | Yes, after installing `linux-modules-extra-$(uname -r)`. The stock cloud-image kernel doesn't include it. | `environment.txt` |
| Frame crosses `vcan` | Yes. `cansend` sent `123#DEADBEEF01020304`; a separate `candump` recorded it. | `vcan_capture.txt` |
| Lab broker starts from `config/mosquitto-lab.conf` | Yes, Mosquitto 2.0.18 on a private local port. | `puback_broker_log.txt` |
| When libmosquitto sends PUBACK | **Before `on_message` returns.** The handler held the message from :10 to :14; the broker logged the PUBACK at :10. | `puback_probe.txt`, `puback_broker_log.txt` |

## What the PUBACK result means

The broker treats a message as delivered as soon as the collector's library acknowledges it, which happens before the collector has saved it. If the collector dies while handling a message, that one message is lost; the broker will not resend it.

Decision (plan item 4): keep libmosquitto and document this as a loss window of at most one message per collector crash. Messages the collector has not yet read are not acknowledged, so the broker resends those after the collector reconnects with its persistent session. The `collector_before_persist` crash test checks this exact prediction.

## CI

The lab is not pushed anywhere, so no hosted CI runner was tested. T6 uses a documented Linux runner instead: the Lima VM, created by `scripts/lima/create_vm.sh`.
