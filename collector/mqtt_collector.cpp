// Subscribes to lab telemetry over MQTT, saves each valid reading in the
// checked store, and only then publishes an application receipt.
//
//   mqtt_collector --wal store.wal [--broker-host H] [--broker-port P]
//                  [--client-id collector] [--trace path.jsonl]
//
// One thread: the MQTT network loop runs here and the message handler saves
// synchronously, so the store has exactly one owner.
//
// Test builds (LAB_CRASH_HOOKS) stop with SIGKILL at a named point when
// LAB_CRASH_AT is collector_before_persist or collector_after_persist, on the
// LAB_CRASH_AFTER-th message (default 1).

#include "MetricsStore.h"
#include "telemetry_message.h"

#include <mosquitto.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace
{
volatile std::sig_atomic_t gStop = 0;

void OnSignal(int)
{
    gStop = 1;
}

struct Options
{
    std::string host = "127.0.0.1";
    int port = 1883;
    std::string walPath;
    std::string clientId = "collector";
    std::string tracePath;
};

bool ParseOptions(int argc, char *argv[], Options &options)
{
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string arg = argv[i];
        const std::string value = argv[i + 1];
        if (arg == "--broker-host")
        {
            options.host = value;
        }
        else if (arg == "--broker-port")
        {
            options.port = std::stoi(value);
        }
        else if (arg == "--wal")
        {
            options.walPath = value;
        }
        else if (arg == "--client-id")
        {
            options.clientId = value;
        }
        else if (arg == "--trace")
        {
            options.tracePath = value;
        }
        else
        {
            return false;
        }
    }
    return argc % 2 == 1 && !options.walPath.empty();
}

class Trace
{
public:
    explicit Trace(const std::string &path) : file_(path.empty() ? nullptr : std::fopen(path.c_str(), "a")) {}
    ~Trace()
    {
        if (file_)
        {
            std::fclose(file_);
        }
    }

    void write(nlohmann::json event)
    {
        if (!file_)
        {
            return;
        }
        event["t_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        std::fprintf(file_, "%s\n", event.dump().c_str());
        std::fflush(file_);
    }

private:
    std::FILE *file_;
};

class CrashPoint
{
public:
    CrashPoint()
    {
#ifdef LAB_CRASH_HOOKS
        if (const char *at = std::getenv("LAB_CRASH_AT"))
        {
            name_ = at;
        }
        if (const char *after = std::getenv("LAB_CRASH_AFTER"))
        {
            after_ = std::strtoul(after, nullptr, 10);
        }
#endif
    }

    void reach(const char *point, Trace &trace)
    {
        if (name_.empty() || name_ != point || ++count_ < after_)
        {
            return;
        }
        trace.write({{"event", "crash_point"}, {"point", point}});
        std::raise(SIGKILL);
    }

private:
    std::string name_;
    unsigned long after_ = 1;
    unsigned long count_ = 0;
};

struct Collector
{
    MetricsStore store;
    Trace trace;
    CrashPoint crash;
};

nlohmann::json Identity(const lab::ParsedTelemetry &message)
{
    if (!message.reading.id)
    {
        return nullptr;
    }
    return {{"run_id", message.reading.id->runId},
            {"gateway_epoch", message.reading.id->epoch},
            {"sequence", message.reading.id->sequence}};
}

void SendReceipt(mosquitto *client, Collector &collector, const lab::ParsedTelemetry &message,
                 const std::string &payload)
{
    const std::string topic = lab::ReceiptTopic(message.runId);
    const int rc = mosquitto_publish(client, nullptr, topic.c_str(), static_cast<int>(payload.size()),
                                     payload.data(), 1, false);
    if (rc != MOSQ_ERR_SUCCESS)
    {
        collector.trace.write({{"event", "receipt_failed"}, {"id", Identity(message)}, {"rc", rc}});
    }
}

void OnConnect(mosquitto *client, void *context, int rc)
{
    auto *collector = static_cast<Collector *>(context);
    collector->trace.write({{"event", "connected"}, {"rc", rc}});
    if (rc == 0)
    {
        mosquitto_subscribe(client, nullptr, lab::kTelemetryFilter, 1);
    }
}

void OnSubscribe(mosquitto *, void *context, int, int, const int *)
{
    static_cast<Collector *>(context)->trace.write({{"event", "subscribed"}});
}

void OnDisconnect(mosquitto *, void *context, int rc)
{
    static_cast<Collector *>(context)->trace.write({{"event", "disconnected"}, {"rc", rc}});
}

void OnMessage(mosquitto *client, void *context, const mosquitto_message *message)
{
    auto &collector = *static_cast<Collector *>(context);
    const std::string payload(static_cast<const char *>(message->payload),
                              message->payloadlen > 0 ? static_cast<size_t>(message->payloadlen) : 0u);
    const lab::ParsedTelemetry parsed = lab::ParseTelemetry(message->topic, payload);
    if (!parsed.ok)
    {
        collector.trace.write(
            {{"event", "rejected"}, {"topic", message->topic}, {"reason", parsed.reason}, {"id", Identity(parsed)}});
        if (parsed.identityKnown)
        {
            SendReceipt(client, collector, parsed, lab::RejectionPayload(parsed));
        }
        return;
    }

    collector.crash.reach("collector_before_persist", collector.trace);
    const IngestResult result = collector.store.ingest(parsed.reading);
    collector.trace.write({{"event", Describe(result.status)},
                           {"id", Identity(parsed)},
                           {"signal", parsed.reading.signal},
                           {"message", result.message}});
    collector.crash.reach("collector_after_persist", collector.trace);

    // The receipt goes out only now, after the store has written and synced
    // the record (or reported why it didn't).
    SendReceipt(client, collector, parsed, lab::ReceiptPayload(parsed, result));
}
} // namespace

int main(int argc, char *argv[])
{
    Options options;
    try
    {
        if (!ParseOptions(argc, argv, options))
        {
            std::fprintf(stderr, "usage: %s --wal PATH [--broker-host H] [--broker-port P] [--client-id ID] "
                                 "[--trace PATH]\n",
                         argv[0]);
            return 1;
        }
    }
    catch (const std::exception &)
    {
        std::fprintf(stderr, "invalid --broker-port\n");
        return 1;
    }

    std::signal(SIGTERM, OnSignal);
    std::signal(SIGINT, OnSignal);

    Collector collector{MetricsStore(options.walPath), Trace(options.tracePath), CrashPoint()};
    const StoreResult opened = collector.store.startupRecover();
    collector.trace.write({{"event", opened.ok ? "recovered" : "recovery_failed"},
                           {"records", opened.records},
                           {"discarded_tail_bytes", opened.discardedTailBytes},
                           {"message", opened.message}});
    if (!opened.ok)
    {
        std::fprintf(stderr, "error: %s\n", opened.message.c_str());
        return 2;
    }

    mosquitto_lib_init();
    // Persistent session: the broker keeps QoS 1 messages for us while we're down.
    mosquitto *client = mosquitto_new(options.clientId.c_str(), false, &collector);
    mosquitto_connect_callback_set(client, OnConnect);
    mosquitto_subscribe_callback_set(client, OnSubscribe);
    mosquitto_disconnect_callback_set(client, OnDisconnect);
    mosquitto_message_callback_set(client, OnMessage);

    bool connected = mosquitto_connect(client, options.host.c_str(), options.port, 30) == MOSQ_ERR_SUCCESS;
    while (!gStop)
    {
        if (!connected)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            connected = mosquitto_reconnect(client) == MOSQ_ERR_SUCCESS;
            continue;
        }
        if (mosquitto_loop(client, 100, 1) != MOSQ_ERR_SUCCESS)
        {
            connected = false;
        }
    }

    mosquitto_disconnect(client);
    mosquitto_destroy(client);
    mosquitto_lib_cleanup();
    collector.store.shutdown();
    collector.trace.write({{"event", "stopped"}});
    return 0;
}
