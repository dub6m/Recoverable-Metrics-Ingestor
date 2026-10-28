#include "MetricsStore.h"

#include <cstring>

namespace
{
bool SameBits(double a, double b)
{
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}
} // namespace

const char *Describe(IngestStatus status)
{
    switch (status)
    {
    case IngestStatus::Stored:
        return "stored";
    case IngestStatus::Duplicate:
        return "duplicate";
    case IngestStatus::Conflict:
        return "conflict";
    case IngestStatus::Invalid:
        return "invalid";
    case IngestStatus::Failed:
        return "failed";
    }
    return "unknown";
}

MetricsStore::MetricsStore(const std::string &walPath, FileOps ops) : wal_(walPath, std::move(ops)), memStore_() {}

StoreResult MetricsStore::startupRecover()
{
    return open(WriteAheadLog::Mode::ReadWrite);
}

StoreResult MetricsStore::openReadOnly()
{
    return open(WriteAheadLog::Mode::ReadOnly);
}

StoreResult MetricsStore::open(WriteAheadLog::Mode mode)
{
    if (opened_)
    {
        return *opened_;
    }

    WriteAheadLog::OpenInfo info;
    const WalResult result = wal_.open(
        mode, [this](const Reading &reading, size_t) { return remember(reading); }, info);

    StoreResult opened;
    opened.ok = result.ok;
    opened.message = result.message;
    opened.records = info.records;
    opened.discardedTailBytes = info.discardedTailBytes;
    if (!result.ok)
    {
        memStore_ = MemStore();
        readings_.clear();
        byIdentity_.clear();
    }
    writable_ = result.ok && mode == WriteAheadLog::Mode::ReadWrite;
    opened_ = opened;
    return opened;
}

WalResult MetricsStore::remember(const Reading &reading)
{
    if (reading.id)
    {
        const auto inserted = byIdentity_.emplace(reading.id->key(), readings_.size());
        if (!inserted.second)
        {
            return WalResult::Error("duplicate reading identity " + reading.id->key());
        }
    }
    memStore_.append(reading.signal, reading.timestamp, reading.value);
    readings_.push_back(reading);
    return WalResult::Ok();
}

IngestResult MetricsStore::ingest(
    const std::string &metricName,
    int64_t timestamp,
    double value)
{
    Reading reading;
    reading.signal = metricName;
    reading.timestamp = timestamp;
    reading.value = value;
    return ingest(reading);
}

IngestResult MetricsStore::ingest(const Reading &reading)
{
    if (!writable_)
    {
        return {IngestStatus::Failed, "store is not open for writing"};
    }

    std::string why;
    if (!ValidateReading(reading, why))
    {
        return {IngestStatus::Invalid, why};
    }

    if (reading.id)
    {
        const auto existing = byIdentity_.find(reading.id->key());
        if (existing != byIdentity_.end())
        {
            const Reading &stored = readings_[existing->second];
            if (stored.signal == reading.signal && stored.timestamp == reading.timestamp &&
                SameBits(stored.value, reading.value))
            {
                return {IngestStatus::Duplicate, "already stored"};
            }
            return {IngestStatus::Conflict, "identity " + reading.id->key() + " already stored with a different reading"};
        }
    }

    // Log first. Memory changes only after the record is written and synced.
    const WalResult written = wal_.append(reading);
    if (!written.ok)
    {
        return {IngestStatus::Failed, written.message};
    }
    remember(reading);
    return {IngestStatus::Stored, ""};
}

std::deque<MetricSample> MetricsStore::query(
    const std::string &metricName,
    int64_t sinceTimestamp)
{
    return memStore_.query(metricName, sinceTimestamp);
}

void MetricsStore::shutdown()
{
    wal_.close();
    writable_ = false;
}
