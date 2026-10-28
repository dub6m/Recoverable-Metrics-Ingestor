# T4 — broker behaviour probes (2026-10-27)

Two short probes, run in the Ubuntu VM with Mosquitto 2.0.18 and paho-mqtt 2.1.0. Scripts are in `scripts/t4/`; output is in this folder.

## paho-mqtt during a broker outage (`paho_outage_probe.txt`)

The publisher's queue was limited to 5 messages, and the broker had no persistence.

- During the outage, the first 5 QoS 1 publishes returned `NO_CONN`: paho queued them. The next 3 returned `QUEUE_SIZE`: rejected, with a clear signal. The gateway writes those to its drop log.
- After the broker came back, paho sent the 5 queued messages and got broker acknowledgements for all of them.
- **But the subscriber received none of them.** The restarted broker had forgotten the subscriber's persistent session and subscription. The messages arrived before the subscriber subscribed again, so the broker discarded them.

## Broker persistence (`broker_restart_probe.txt`)

Same test, with `persistence true`, `autosave_interval 1` and `autosave_on_changes true`. The subscriber reconnects slowly (1 s), so queued messages have to wait in its saved session.

- Graceful restart (SIGTERM): all 5 messages sent during the outage were delivered.
- Killed broker (SIGKILL): all 5 were delivered too. The session was saved when it was created, and the messages arrived after the restart.

## Change from the build plan

The plan said to keep Mosquitto persistence off and treat a broker restart as a loss window. The first probe shows that window would cover everything the gateway buffered during the outage, not just messages caught mid-delivery. `config/mosquitto-lab.conf` now turns persistence on and saves on every change. What remains is a smaller window: a killed broker can lose messages it had accepted but not yet saved or delivered.
