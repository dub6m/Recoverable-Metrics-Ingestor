#pragma once

#include <string>
#include <unordered_map>
#include <deque>
#include <cstdint>

struct MetricSample
{
    int64_t timestamp;
    double value;
};

class MemStore
{
public:
    void append(
        const std::string &metricName,
        int64_t timestamp,
        double value);

    // Get samples newer than sinceTimestamp (most recent first)
    std::deque<MetricSample> query(
        const std::string &metricName,
        int64_t sinceTimestamp);

    // Drop old samples (retention)
    void evictOlderThan(int64_t cutoffTimestamp);

private:
    std::unordered_map<
        std::string,
        std::deque<MetricSample>>
        store_;
};
