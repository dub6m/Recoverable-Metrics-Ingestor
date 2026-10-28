# T4 probe: with broker persistence saved on every change, does a persistent
# subscriber get messages queued during a graceful (SIGTERM) or killed (SIGKILL) broker restart?
import subprocess, time, sys, os, signal, tempfile
import paho.mqtt.client as mqtt
port = 18842
d = tempfile.mkdtemp()
conf = os.path.join(d, "b.conf")
open(conf, "w").write(f"listener {port} 127.0.0.1\nallow_anonymous true\npersistence true\npersistence_location {d}/\nautosave_interval 1\nautosave_on_changes true\n")
def broker():
    p = subprocess.Popen(["mosquitto", "-c", conf], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); time.sleep(0.3); return p
def run(stop_signal):
    b = broker()
    got = []
    sub = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"sub-{stop_signal}", clean_session=False)
    sub.on_message = lambda c, u, m: got.append(m.payload.decode())
    sub.on_connect = lambda c, u, f, rc, p: c.subscribe("probe/#", 1)
    sub.reconnect_delay_set(1, 1)  # slow re-subscribe: queued messages must wait in the session
    sub.connect("127.0.0.1", port); sub.loop_start()
    pub = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"pub-{stop_signal}")
    pub.max_queued_messages_set(5); pub.reconnect_delay_set(0.1, 0.2)
    pub.connect("127.0.0.1", port); pub.loop_start(); time.sleep(0.5)
    pub.publish("probe/x", "before", qos=1); time.sleep(0.3)
    b.send_signal(stop_signal); b.wait(); time.sleep(0.2)
    for i in range(5): pub.publish("probe/x", f"during{i}", qos=1)
    b = broker(); time.sleep(3)
    print(signal.Signals(stop_signal).name, "got", got)
    pub.loop_stop(); sub.loop_stop(); b.terminate(); b.wait()
run(signal.SIGTERM)
run(signal.SIGKILL)
