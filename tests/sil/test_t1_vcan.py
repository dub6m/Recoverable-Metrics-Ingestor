"""T1: an actual CAN frame crosses a vcan interface, recorded by an independent receiver."""

import can

import lab


def test_frame_crosses_vcan_and_candump_records_it(vcan, logs):
    candump = lab.Process("candump", ["candump", "-L", vcan], logs).start()
    try:
        lab.wait_until(lambda: candump.running(), 2, "candump to start")
        bus = can.Bus(interface="socketcan", channel=vcan)
        try:
            message = can.Message(arbitration_id=0x123, data=bytes.fromhex("DEADBEEF01020304"),
                                  is_extended_id=False)
            # candump may need a moment to bind; resend until it reports the frame.
            def seen():
                bus.send(message)
                return f"{vcan} 123#DEADBEEF01020304" in candump.log()
            lab.wait_until(seen, 3, "candump to record frame 0x123", interval=0.1)
        finally:
            bus.shutdown()
    finally:
        candump.stop()
