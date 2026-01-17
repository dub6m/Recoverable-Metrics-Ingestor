#include "MemStore.h"

void MemStore::append(
    const std::string &metricName,
    int64_t timestamp,
    double value)
{
    store_[metricName].push_back(MetricSample{timestamp, value});
}

std::deque<MetricSample> MemStore::query(
    const std::string &metricName,
    int64_t sinceTimestamp)
{
    std::deque<MetricSample> results;
    auto iter = store_.find(metricName);
    if (iter == store_.end())
    {
        return results;
    }

    const auto &samples = iter->second;
    for (auto sampleIter = samples.rbegin(); sampleIter != samples.rend(); ++sampleIter)
    {
        if (sampleIter->timestamp >= sinceTimestamp)
        {
            results.push_back(*sampleIter);
        }
    }

    return results;
}

void MemStore::evictOlderThan(int64_t cutoffTimestamp)
{
    for (auto iter = store_.begin(); iter != store_.end();)
    {
        auto &samples = iter->second;
        while (!samples.empty() && samples.front().timestamp < cutoffTimestamp)
        {
            samples.pop_front();
        }

        if (samples.empty())
        {
            iter = store_.erase(iter);
        }
        else
        {
            ++iter;
        }
    }
}
