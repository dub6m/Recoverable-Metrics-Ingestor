#include "MetricsStore.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
void PrintUsage()
{
    std::cout << "Usage:\n"
              << "  metrics_cli <wal_path> ingest <metric> <timestamp> <value>\n"
              << "  metrics_cli <wal_path> query <metric> <since_timestamp>\n";
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

bool ParseDouble(const std::string &input, double &value)
{
    try
    {
        size_t consumed = 0;
        value = std::stod(input, &consumed);
        return consumed == input.size();
    }
    catch (const std::exception &)
    {
        return false;
    }
}
} // namespace

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        PrintUsage();
        return 1;
    }

    const std::string walPath = argv[1];
    if (argc < 3)
    {
        PrintUsage();
        return 1;
    }

    const std::string command = argv[2];
    MetricsStore store(walPath);
    store.startupRecover();

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
        if (!ParseInt64(argv[4], timestamp) || !ParseDouble(argv[5], value))
        {
            std::cerr << "Invalid timestamp or value.\n";
            return 1;
        }

        store.ingest(metricName, timestamp, value);
        store.shutdown();
        std::cout << "OK\n";
        return 0;
    }

    if (command == "query")
    {
        if (argc != 5)
        {
            PrintUsage();
            return 1;
        }

        const std::string metricName = argv[3];
        int64_t sinceTimestamp = 0;
        if (!ParseInt64(argv[4], sinceTimestamp))
        {
            std::cerr << "Invalid since_timestamp.\n";
            return 1;
        }

        const auto samples = store.query(metricName, sinceTimestamp);
        store.shutdown();
        std::cout << "Samples (most recent first):\n";
        for (const auto &sample : samples)
        {
            std::cout << sample.timestamp << " " << sample.value << "\n";
        }
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    PrintUsage();
    return 1;
}
