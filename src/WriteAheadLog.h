#pragma once

#include "FileOps.h"
#include "Record.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

struct WalResult
{
    bool ok = true;
    std::string message;

    static WalResult Ok() { return {}; }
    static WalResult Error(std::string message) { return {false, std::move(message)}; }
};

// Append-only log of readings. File format (version 2):
//
//   METRICSWAL 2\n
//   <payload length> <crc32 hex> <payload>\n   (one line per reading)
//
// A reading counts as written only after its whole line is written and synced.
class WriteAheadLog
{
public:
    enum class Mode
    {
        ReadWrite, // creates the file, takes an exclusive lock, may cut off an incomplete final record
        ReadOnly,  // never changes the file; ignores an incomplete final record
    };

    struct OpenInfo
    {
        size_t records = 0;
        size_t discardedTailBytes = 0;
    };

    using RecordCallback = std::function<WalResult(const Reading &reading, size_t line)>;

    explicit WriteAheadLog(std::string filePath, FileOps ops = PosixFileOps());
    ~WriteAheadLog();
    WriteAheadLog(const WriteAheadLog &) = delete;
    WriteAheadLog &operator=(const WriteAheadLog &) = delete;

    // Reads every complete record in order. Refuses old-format logs and any
    // corrupt complete record, wherever it is.
    WalResult open(Mode mode, const RecordCallback &onRecord, OpenInfo &info);

    // Writes and syncs one record. On a write error the partial record is
    // removed; if that or a sync fails, the log stops accepting writes.
    WalResult append(const Reading &reading);

    void close();
    bool failed() const { return failed_; }

private:
    WalResult writeAll(const std::string &bytes);
    WalResult fail(const std::string &message);

    std::string filePath_;
    FileOps ops_;
    int fd_ = -1;
    Mode mode_ = Mode::ReadOnly;
    uint64_t size_ = 0; // bytes of complete records, including the header
    bool failed_ = false;
};
