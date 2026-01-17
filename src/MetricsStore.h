#pragma once

#include "MemStore.h"
#include "WriteAheadLog.h"

#include <cstdint>
#include <deque>
#include <string>

class MetricsStore
{
public:
    explicit MetricsStore(const std::string &walPath);

    void startupRecover();

    void ingest(
        const std::string &metricName,
        int64_t timestamp,
        double value);

    std::deque<MetricSample> query(
        const std::string &metricName,
        int64_t sinceTimestamp);

    void shutdown();

private:
    WriteAheadLog wal_;
    MemStore memStore_;
};
