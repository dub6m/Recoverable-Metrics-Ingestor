#include "MetricsStore.h"

#include <cstdlib>
#include <iostream>
#include <string>

// Exit codes: 0 success, 1 usage or invalid input, 2 storage error.
namespace
{
void PrintUsage()
{
    std::cout << "Usage:\n"
              << "  metrics_cli <wal_path> ingest <metric> <timestamp> <value>\n"
              << "  metrics_cli <wal_path> query <metric> <since_timestamp>\n"
              << "  metrics_cli <wal_path> dump\n";
}

bool ParseInt64(const std::string &input, int64_t &value)
{
    try
    {
        size_t consumed = 0;
        value = std::stoll(input, &consumed);
        return consumed == input.size();
    }
    catch (const std::exception &)
    {
        return false;
    }
}

std::string JsonString(const std::string &token)
{
    // Tokens are restricted to letters, digits and . _ : - so no escaping is needed.
    return "\"" + token + "\"";
}

// JSON readers treat "-0" or "12" as integers, losing the sign of zero; always
// print a floating-point number.
std::string JsonNumber(double value)
{
    std::string text = FormatValue(value);
    if (text.find_first_of(".e") == std::string::npos)
    {
        text += ".0";
    }
    return text;
}

void PrintReadingJson(const Reading &reading)
{
    std::cout << "{\"run_id\":" << (reading.id ? JsonString(reading.id->runId) : "null")
              << ",\"gateway_epoch\":" << (reading.id ? JsonString(reading.id->epoch) : "null")
              << ",\"sequence\":" << (reading.id ? std::to_string(reading.id->sequence) : "null")
              << ",\"signal\":" << JsonString(reading.signal) << ",\"timestamp\":" << reading.timestamp
              << ",\"value\":" << JsonNumber(reading.value) << "}\n";
}
} // namespace

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        PrintUsage();
        return 1;
    }

    const std::string walPath = argv[1];
    const std::string command = argv[2];
    MetricsStore store(walPath);

    if (command == "ingest")
    {
        if (argc != 6)
        {
            PrintUsage();
            return 1;
        }

        const std::string metricName = argv[3];
        int64_t timestamp = 0;
        double value = 0.0;
        if (!ParseInt64(argv[4], timestamp) || !ParseValue(argv[5], value))
        {
            std::cerr << "Invalid timestamp or value.\n";
            return 1;
        }

        const StoreResult opened = store.startupRecover();
        if (!opened.ok)
        {
            std::cerr << "error: " << opened.message << "\n";
            return 2;
        }
        const IngestResult result = store.ingest(metricName, timestamp, value);
        store.shutdown();
        if (result.status == IngestStatus::Invalid)
        {
            std::cerr << "Invalid reading: " << result.message << "\n";
            return 1;
        }
        if (!result.accepted())
        {
            std::cerr << "error: " << result.message << "\n";
            return 2;
        }
        std::cout << "OK\n";
        return 0;
    }

    if (command == "query" || command == "dump")
    {
        int64_t sinceTimestamp = 0;
        const bool isQuery = command == "query";
        if ((isQuery && (argc != 5 || !ParseInt64(argv[4], sinceTimestamp))) || (!isQuery && argc != 3))
        {
            PrintUsage();
            return 1;
        }

        // Read-only: safe while another process (the collector) is writing.
        const StoreResult opened = store.openReadOnly();
        if (!opened.ok)
        {
            std::cerr << "error: " << opened.message << "\n";
            return 2;
        }

        if (isQuery)
        {
            const auto samples = store.query(argv[3], sinceTimestamp);
            std::cout << "Samples (most recent first):\n";
            for (const auto &sample : samples)
            {
                std::cout << sample.timestamp << " " << FormatValue(sample.value) << "\n";
            }
        }
        else
        {
            for (const auto &reading : store.readings())
            {
                PrintReadingJson(reading);
            }
        }
        store.shutdown();
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    PrintUsage();
    return 1;
}
