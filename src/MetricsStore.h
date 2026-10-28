#pragma once

#include "MemStore.h"
#include "Record.h"
#include "WriteAheadLog.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct StoreResult
{
    bool ok = true;
    std::string message;
    size_t records = 0;            // records read from the log
    size_t discardedTailBytes = 0; // incomplete final record cut off (or ignored, read-only)
};

enum class IngestStatus
{
    Stored,    // written and synced, then added to memory
    Duplicate, // same identity and same reading already stored; nothing written
    Conflict,  // same identity, different reading; rejected
    Invalid,   // bad name or non-finite value; rejected
    Failed,    // storage error; nothing accepted
};

struct IngestResult
{
    IngestStatus status = IngestStatus::Failed;
    std::string message;

    bool accepted() const { return status == IngestStatus::Stored || status == IngestStatus::Duplicate; }
};

const char *Describe(IngestStatus status);

class MetricsStore
{
public:
    explicit MetricsStore(const std::string &walPath, FileOps ops = PosixFileOps());

    // Opens the log for writing (creating it if needed), cuts off an
    // incomplete final record and replays every record into memory. One-shot:
    // later calls return the first result without replaying again.
    StoreResult startupRecover();

    // Opens the log without changing it, e.g. to query while a collector writes.
    StoreResult openReadOnly();

    // Original interface: a reading without identity (never deduplicated).
    IngestResult ingest(
        const std::string &metricName,
        int64_t timestamp,
        double value);

    // The reading is accepted only after its log record is written and synced.
    IngestResult ingest(const Reading &reading);

    std::deque<MetricSample> query(
        const std::string &metricName,
        int64_t sinceTimestamp);

    // Every accepted reading in arrival (log) order.
    const std::vector<Reading> &readings() const { return readings_; }

    void shutdown();

private:
    StoreResult open(WriteAheadLog::Mode mode);
    WalResult remember(const Reading &reading);

    WriteAheadLog wal_;
    MemStore memStore_;
    std::vector<Reading> readings_;
    std::unordered_map<std::string, size_t> byIdentity_;
    std::optional<StoreResult> opened_;
    bool writable_ = false;
};
