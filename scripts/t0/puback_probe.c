// T0 probe: does libmosquitto send PUBACK for a QoS 1 message before or after
// on_message returns? The handler sleeps for HOLD_SECONDS; the broker log
// (log_type all) shows when it received the PUBACK. Compare the timestamps.
#include <mosquitto.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define HOLD_SECONDS 4

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void on_connect(struct mosquitto *m, void *ud, int rc)
{
    (void)ud;
    printf("%.3f connected rc=%d\n", now(), rc);
    mosquitto_subscribe(m, NULL, "t0/probe", 1);
}

static void on_subscribe(struct mosquitto *m, void *ud, int mid, int n, const int *q)
{
    (void)m; (void)ud; (void)mid; (void)n;
    printf("%.3f subscribed qos=%d\n", now(), q[0]);
    fflush(stdout);
}

static void on_message(struct mosquitto *m, void *ud, const struct mosquitto_message *msg)
{
    (void)ud;
    printf("%.3f on_message start mid=%d qos=%d\n", now(), msg->mid, msg->qos);
    fflush(stdout);
    sleep(HOLD_SECONDS);
    printf("%.3f on_message end\n", now());
    fflush(stdout);
    mosquitto_disconnect(m);
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 18830;
    int major, minor, rev;
    mosquitto_lib_version(&major, &minor, &rev);
    printf("libmosquitto %d.%d.%d\n", major, minor, rev);
    mosquitto_lib_init();
    struct mosquitto *m = mosquitto_new("t0-probe", true, NULL);
    mosquitto_connect_callback_set(m, on_connect);
    mosquitto_subscribe_callback_set(m, on_subscribe);
    mosquitto_message_callback_set(m, on_message);
    if (mosquitto_connect(m, "127.0.0.1", port, 30) != MOSQ_ERR_SUCCESS)
    {
        fprintf(stderr, "connect failed\n");
        return 1;
    }
    mosquitto_loop_forever(m, 1000, 1);
    mosquitto_destroy(m);
    mosquitto_lib_cleanup();
    return 0;
}
