# T4 probe: does paho-mqtt queue QoS 1 messages while the broker is down, and
# does its queue limit report drops? (No broker persistence here.)
import subprocess, time, socket, sys, json
import paho.mqtt.client as mqtt
port = 18841
conf = "/tmp/paho_probe.conf"
open(conf, "w").write(f"listener {port} 127.0.0.1\nallow_anonymous true\n")
def broker(): 
    p = subprocess.Popen(["mosquitto", "-c", conf], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); time.sleep(0.3); return p
b = broker()
got = []
sub = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="probe-sub", clean_session=False)
sub.on_message = lambda c, u, m: got.append(m.payload.decode())
sub.on_connect = lambda c, u, f, rc, p: c.subscribe("probe/#", 1)
sub.connect("127.0.0.1", port); sub.loop_start()
acked = []
pub = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="probe-pub")
pub.on_publish = lambda c, u, mid, rc, p: acked.append(mid)
pub.max_queued_messages_set(5)
pub.reconnect_delay_set(0.1, 0.5)
pub.connect("127.0.0.1", port); pub.loop_start(); time.sleep(0.3)
r = pub.publish("probe/x", "before", qos=1); time.sleep(0.2)
print("before: rc", r.rc, "acked", len(acked), "got", got)
b.terminate(); b.wait(); time.sleep(0.3)
rcs = [pub.publish("probe/x", f"during{i}", qos=1).rc for i in range(8)]
print("during outage rcs:", rcs, "(0=SUCCESS, 4=NO_CONN, 15=QUEUE_SIZE)")
b = broker(); time.sleep(2.0)
print("after restart: acked", len(acked), "got", got)
pub.loop_stop(); sub.loop_stop(); b.terminate()
