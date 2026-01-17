#include "MetricsStore.h"

MetricsStore::MetricsStore(const std::string &walPath) : wal_(walPath), memStore_() {}

void MetricsStore::startupRecover()
{
    wal_.replay([this](const std::string &metricName, int64_t timestamp, double value) {
        memStore_.append(metricName, timestamp, value);
    });
}

void MetricsStore::ingest(
    const std::string &metricName,
    int64_t timestamp,
    double value)
{
    wal_.append(metricName, timestamp, value);
    wal_.flush();
    memStore_.append(metricName, timestamp, value);
}

std::deque<MetricSample> MetricsStore::query(
    const std::string &metricName,
    int64_t sinceTimestamp)
{
    return memStore_.query(metricName, sinceTimestamp);
}

void MetricsStore::shutdown()
{
    wal_.flush();
    wal_.close();
}
